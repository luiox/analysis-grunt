/// @file        number_fold.cpp
/// @brief       残留 `number_fold` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/number_fold.hpp"

#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/iter_budget.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

#include "jsim/value.hpp"

namespace grunt::number_fold {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using const_args::ValueLoad;
using proof_tools::roundTripSafe;

/// 基 32 数字：0-9 = 0..9，a-v / A-V = 10..31（java.lang.Long.toString(v, 32)
/// 的生成口径）。
int base32Digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'v') return c - 'a' + 10;
    if (c >= 'A' && c <= 'V') return c - 'A' + 10;
    return -1;
}

/// @brief 严格基 32 解析：java.lang.Long.parseUnsignedLong 的生成侧逆。
///
/// 只认无符号数字前缀串（生成侧 Long.toString(v, 32) 不产出符号/空白），
/// 任一字符越出 radix 或总和超 64 位即拒绝（fail-closed）。
bool parseBase32(std::string_view text, ca::u64& out)
{
    if (text.empty()) return false;
    ca::u64 acc = 0;
    for (const char c : text) {
        const int d = base32Digit(c);
        if (d < 0) return false;
        const ca::u64 limit = (static_cast<ca::u64>(-1) - static_cast<ca::u64>(d)) / 32;
        if (acc > limit) return false;
        acc = acc * 32 + static_cast<ca::u64>(d);
    }
    out = acc;
    return true;
}

/// @brief 包装调用的引用判据：INVOKESTATIC 且 owner/name/desc 精确吻合。
bool isWrapperInvoke(ir::ClassSession& s, const ir::Insn& in, std::string_view owner,
                     std::string_view name, std::string_view desc)
{
    if (in.op != Op::INVOKESTATIC) return false;
    ir::ClassSession::MemberSem sem;
    if (!s.memberOf(in, sem)) return false;
    return sem.kind == ir::ClassSession::MemberSem::Kind::Method
           && sem.owner == owner && sem.name == name && sem.desc == desc;
}

/// @brief 跨度：order 区间 [firstPos..lastPos] + 折叠值 + 尾序号。
struct Span
{
    ca::usize firstPos = 0;   ///< 锚点（跨度首条装载）在 order 里的位置
    ca::usize lastPos  = 0;   ///< 跨度尾条在 order 里的位置
    ca::usize tailIdx  = 0;   ///< 尾条在实参序号（收集序）里的下标
    ValueLoad value;          ///< 折叠结果
};

constexpr ca::usize kMaxSpan = 7;   ///< 最长跨度（int 跨度 6 条 + float 包装 1 条）

/// @brief 从 from 起收集最多 kMaxSpan 条非 NOP 指令（NOP 是 squeeze 遗留落点，
///        不驻栈，跳过——与 const_args::collectArgs 同一口径）。
void collectReal(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                 ca::usize from, std::vector<ca::usize>& pos,
                 std::vector<const ir::Insn*>& in)
{
    pos.clear();
    in.clear();
    for (ca::usize p = from; p < order.size() && in.size() < kMaxSpan; ++p) {
        const ir::Insn* cur = dom.peek(order[p]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        pos.push_back(p);
        in.push_back(cur);
    }
}

// ---- 跨度形状（逐条钉死；三式的锚点类型互斥，站点跨度天然不重叠）----------

/// int 跨度 1：<i32 a> I2L <i32 b> I2L LXOR L2I → a ^ b。
///
/// I2L/L2I 走廊是纯噪声：符号扩展保低 32 位，XOR 按位运算，截断回 i32 与
/// plain 异或等价。判据仍按跨度语义取值（先符号扩展再异或再截断）。
bool matchIntViaLong(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                     const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 6) return false;
    ValueLoad b;
    if (in[1]->op != Op::I2L) return false;
    if (!const_args::decodeValueLoad(s, *in[2], b) || b.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (in[3]->op != Op::I2L || in[4]->op != Op::LXOR || in[5]->op != Op::L2I) return false;
    const ca::i64 x = static_cast<ca::i64>(out.value.i32());
    const ca::i64 y = static_cast<ca::i64>(b.i32());
    out.firstPos = pos[0];
    out.lastPos  = pos[5];
    out.tailIdx  = 5;
    out.value.kind  = ValueLoad::Kind::I32;
    out.value.bits  = static_cast<ca::u64>(static_cast<ca::i64>(x ^ y));
    out.value.slots = 1;
    return true;
}

/// int 跨度 2：<i64 a> L2I <i32 b> IXOR → (int)a ^ b。
bool matchLongToInt(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                    const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 4) return false;
    ValueLoad b;
    if (in[1]->op != Op::L2I) return false;
    if (!const_args::decodeValueLoad(s, *in[2], b) || b.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (in[3]->op != Op::IXOR) return false;
    const ca::i32 x = static_cast<ca::i32>(out.value.i64());
    const ca::i32 y = b.i32();
    out.firstPos = pos[0];
    out.lastPos  = pos[3];
    out.tailIdx  = 3;
    out.value.kind  = ValueLoad::Kind::I32;
    out.value.bits  = static_cast<ca::u64>(static_cast<ca::i64>(x ^ y));
    out.value.slots = 1;
    return true;
}

/// long 跨度：LDC "key" <i32 32> Long.parseUnsignedLong(String,I)J <i64 obf>
/// LXOR → key ^ obf（无符号位形异或）。
bool matchBase32Long(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                     const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 5) return false;
    ValueLoad radix;
    ValueLoad obf;
    if (!const_args::decodeValueLoad(s, *in[1], radix) || radix.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (radix.i32() != 32) return false;   ///< 家族生成面恒 Long.toString(v, 32)
    if (!isWrapperInvoke(s, *in[2], "java/lang/Long", "parseUnsignedLong",
                         "(Ljava/lang/String;I)J")) {
        return false;
    }
    if (!const_args::decodeValueLoad(s, *in[3], obf) || obf.kind != ValueLoad::Kind::I64) {
        return false;
    }
    if (in[4]->op != Op::LXOR) return false;

    ca::u64 key = 0;
    if (!parseBase32(out.value.str, key)) return false;
    out.firstPos = pos[0];
    out.lastPos  = pos[4];
    out.tailIdx  = 4;
    out.value.kind  = ValueLoad::Kind::I64;
    out.value.bits  = key ^ static_cast<ca::u64>(obf.i64());
    out.value.str.clear();
    out.value.slots = 2;
    return true;
}

/// @brief 包装扩展：跨度尾随 Float.intBitsToFloat / Double.longBitsToDouble
///        时，跨度产出从整数值升成位形浮点常量（跨度延伸一条）。
///
/// 类型门是语义事实：intBitsToFloat 只收 i32（int 跨度），longBitsToDouble
/// 只收 i64（long 跨度）——引用吻合而类型不吻合即不扩展。
void extendWrapper(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                   const std::vector<const ir::Insn*>& in, Span& out)
{
    const ca::usize wrapperIdx = out.tailIdx + 1;
    if (in.size() <= wrapperIdx) return;
    const ir::Insn* w = in[wrapperIdx];
    if (out.value.kind == ValueLoad::Kind::I32
        && isWrapperInvoke(s, *w, "java/lang/Float", "intBitsToFloat", "(I)F")) {
        out.lastPos    = pos[wrapperIdx];
        out.value.bits = static_cast<ca::u64>(static_cast<ca::u32>(out.value.i32()));
        out.value.kind = ValueLoad::Kind::F32;
        return;
    }
    if (out.value.kind == ValueLoad::Kind::I64
        && isWrapperInvoke(s, *w, "java/lang/Double", "longBitsToDouble", "(J)D")) {
        out.lastPos    = pos[wrapperIdx];
        out.value.bits = static_cast<ca::u64>(out.value.i64());
        out.value.kind = ValueLoad::Kind::F64;
    }
}

/// @brief 跨度内除锚点外的标签门：任何中段指令带标签 = 合并点（外部到达
///        路径的栈形状与跨度产出耦合），站点放弃。
bool labelBlocked(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                  ca::usize firstPos, ca::usize lastPos)
{
    for (ca::usize p = firstPos + 1; p <= lastPos && p < order.size(); ++p) {
        if (!dom.labelsAt(order[p]).empty()) return true;
    }
    return false;
}

/// @brief 在 anchorPos 上钉跨度形状。锚点类型互斥分派（I32 / I64 / String）。
bool tryMatchAt(ir::ClassSession& s, const ir::CodeDom& dom,
                const std::vector<ir::InsnId>& order, ca::usize anchorPos, Span& span)
{
    std::vector<ca::usize>       pos;
    std::vector<const ir::Insn*> in;
    collectReal(dom, order, anchorPos, pos, in);
    if (in.size() < 2) return false;

    // 锚点必须是常量装载（数值或字符串键）；fconst/dconst、null、Class 等
    // 引用形态不是任何跨度的起点。
    ValueLoad anchor;
    if (!const_args::decodeValueLoad(s, *in[0], anchor)) return false;
    if (anchor.kind != ValueLoad::Kind::I32 && anchor.kind != ValueLoad::Kind::I64
        && anchor.kind != ValueLoad::Kind::Str) {
        return false;
    }

    Span out;
    out.value = anchor;
    bool matched = false;
    if (anchor.kind == ValueLoad::Kind::I32) {
        matched = matchIntViaLong(s, pos, in, out);
    } else if (anchor.kind == ValueLoad::Kind::I64) {
        matched = matchLongToInt(s, pos, in, out);
    } else if (anchor.kind == ValueLoad::Kind::Str) {
        matched = matchBase32Long(s, pos, in, out);
    }
    if (!matched) return false;
    extendWrapper(s, pos, in, out);
    span = out;
    return true;
}

/// @brief ValueLoad → SimValue（makeConstLoad 的输入面）。
jsim::SimValue simOf(const ValueLoad& v)
{
    switch (v.kind) {
    case ValueLoad::Kind::I32: return jsim::SimValue::knownI32(v.i32());
    case ValueLoad::Kind::I64: return jsim::SimValue::knownI64(v.i64());
    case ValueLoad::Kind::F32: return jsim::SimValue::knownF32(v.f32());
    case ValueLoad::Kind::F64: return jsim::SimValue::knownF64(v.f64());
    default: return jsim::SimValue{};
    }
}

/// @brief 单方法 SCAN。@return false = 类级迭代预算超限（调用方放弃该类剩余方法）。
bool scanMethod(ir::ClassSession& s, ca::usize methodIndex, const std::string& uri,
                ScanResult& result, iter_budget::ClassBudget& budget)
{
    if (!ir::hasCode(s, methodIndex)) return true;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return true;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return true;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      pos   = 0;
    while (pos < order.size()) {
        if (!budget.tick()) return false;   ///< 逐位置记账：超限即停，类保守跳过
        const ir::Insn* in = cu->dom.peek(order[pos]);
        if (in == nullptr || in->op == Op::NOP) {
            ++pos;
            continue;
        }
        Span span;
        if (tryMatchAt(s, cu->dom, order, pos, span)
            && !labelBlocked(cu->dom, order, span.firstPos, span.lastPos)) {
            result.sites.push_back(Site{uri, methodIndex, in->srcPc, in->op, span.value});
            pos = span.lastPos + 1;   ///< 跨度内部不再起锚（锚点类型互斥，无嵌套站点）
            continue;
        }
        ++pos;
    }
    return true;
}

/// @brief 单方法 REWRITE。@return 折叠成功的站点数；failed 走出参。
///
/// cu 由 mutateMethodWithFrames 提升好：promote 只发生一次。站点按 srcPc+op
/// 重定位后全量复核（scan 之后字节可能已被同方法前站改写，不盲改），锚点
/// 原地改成结果常量装载，跨度其余非 NOP 指令擦除。
/// @brief 单方法 REWRITE。@return 折叠成功的站点数；failed 走出参。
///
/// cu 由 mutateMethodWithFrames 提升好：promote 只发生一次。站点按 srcPc+op
/// 重定位后全量复核（scan 之后字节可能已被同方法前站改写，不盲改），锚点
/// 原地改成结果常量装载，跨度其余非 NOP 指令擦除。
/// budget 逐站点按方法指令数记账（重定位是线性扫）；超限时剩余站点保守放弃。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ca::usize& failed,
                        ca::usize& skippedByBudget, iter_budget::ClassBudget& budget)
{
    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      hit   = 0;
    ca::usize                      tried = 0;   ///< 本方法已尝试的站点数

    for (const Site* site : sites) {
        // 逐站点按方法指令数记账：重定位 + 复核都是对 order 的线性扫。
        if (!budget.tick(order.size())) {
            skippedByBudget += sites.size() - tried;   ///< 余下站点保守放弃
            return hit;
        }
        ++tried;
        // 重定位：srcPc + op 跨会话稳定（InsnId 不带出去）。
        ca::usize anchorPos = static_cast<ca::usize>(-1);
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const ir::Insn* in = cu->dom.peek(order[pos]);
            if (in != nullptr && in->srcPc == site->anchorSrcPc && in->op == site->anchorOp) {
                anchorPos = pos;
                break;
            }
        }
        if (anchorPos == static_cast<ca::usize>(-1)) {
            ++failed;
            continue;
        }

        // 跨度复核：形状与值都要对上（前站改写后不盲改）。
        Span span;
        if (!tryMatchAt(s, cu->dom, order, anchorPos, span)
            || span.firstPos != anchorPos
            || span.value.kind != site->value.kind || span.value.bits != site->value.bits) {
            ++failed;
            continue;
        }
        // 标签门复核（scan 已拦，防同方法前站改写引入新边界）。
        if (labelBlocked(cu->dom, order, span.firstPos, span.lastPos)) {
            ++failed;
            continue;
        }

        auto slotR = cu->dom.get(order[anchorPos]);
        if (slotR.is_err()) {
            ++failed;
            continue;
        }
        ir::Insn*     anchor   = slotR.unwrap();
        const ir::Insn original = *anchor;

        ir::Insn fresh{};
        if (!rewrite_tools::makeConstLoad(s, simOf(span.value), fresh)) {
            ++failed;
            continue;
        }
        fresh.srcPc = original.srcPc;   ///< 锚点保 srcPc：下游 pcmap/调试行落点不变
        *anchor     = fresh;

        // 跨度其余非 NOP 指令擦除。NOP 是 squeeze 遗留落点，留着无害（DeadCode
        // 收尾）。eraseBatch kRejectIfUsed：受害者被异常表引用时拒收——回滚
        // 锚点，整站放弃，不留「常量已覆盖、跨度还在」的半改写态。
        std::vector<ir::InsnId> victims;
        for (ca::usize p = anchorPos + 1; p <= span.lastPos && p < order.size(); ++p) {
            const ir::Insn* victim = cu->dom.peek(order[p]);
            if (victim == nullptr || victim->op == Op::NOP) continue;
            victims.push_back(order[p]);
        }
        if (!victims.empty() && cu->dom.eraseBatch(victims).is_err()) {
            *anchor = original;
            ++failed;
            continue;
        }
        ++hit;
    }
    return hit;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budgetTemplate)
{
    ScanResult result;
    ca::usize  named = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置记账
        ir::ClassSession&        s      = *session;
        const ir::ClassUnit&     unit   = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!scanMethod(s, mi, std::string(entry.uri), result, budget)) {
                ++result.classesSkippedByBudget;
                iter_budget::reportExhausted("grunt.NumberFold", "scan",
                                             std::string(entry.uri), budget, named);
                return;   ///< 该类剩余方法不再扫（保守跳过）
            }
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budgetTemplate)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    // 站点按 uri 聚类（一遍建索引；此前逐类线性筛全站点的做法在万类大罐上
    // 是 classes × sites 的平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit = 0;
    };
    std::vector<PendingWrite> pending;

    ca::usize named = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        const auto        it = byUri.find(uri);
        if (it == byUri.end()) return;
        const std::vector<const Site*>& sitesOf = it->second;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置记账

        // 前置门：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit             = 0;
        ca::usize failed          = 0;
        ca::usize skippedByBudget = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            dirty.attempted += sitesOfMethod.size();
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    methodHit = rewriteMethod(s, &cu, sitesOfMethod, failed,
                                              skippedByBudget, budget);
                    return methodHit > 0;
                });
            if (!ok) {
                // 帧链/编码失败：该方法整方法放弃（session 内半改写态不外泄）。
                failed += methodHit;
                continue;
            }
            hit += methodHit;
        }
        dirty.failed += failed;
        dirty.skippedByBudget += skippedByBudget;
        if (skippedByBudget > 0) {
            iter_budget::reportExhausted("grunt.NumberFold", "rewrite", uri, budget, named);
        }
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.sites += item.hit;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::number_fold
