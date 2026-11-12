/// @file        string_cache_fold.cpp
/// @brief       残留 `string_cache_fold` 三段实现：形状钉面（S2）→ Era 检查 +
///              TSV 旁证（S3/S1）→ 两形态改写（锚点原地换 ldc + 跨度擦除）。
///
/// 形态、判据分级与锚保留/槽位面设计见头文件；骨架对齐 number_fold（跨度
/// 折叠）与 indy_decode（Era 分支内消费旁证）：scan 只读、rewrite 只写
/// （mutateMethodWithFrames 正统帧链写回）、proof 只判（类数不增 + 脏类可
/// 重解析 + residual 统计）。
///
/// ## TSV 注入面
///
/// 文件读取收敛在 scan 之前：CLI `--tsv` 传路径，adapter 在 probeEra 判为
/// Kasumi 时才读文件；parseTsv 是纯函数（点分 owner → slash，转义反转义），
/// 残留本体零文件系统副作用。TSV 内容不进仓，文档只记来源形态
/// （KasumiStringDumper 四列），不记样本明文。
///
/// ## SCAN
///
/// 粗找 = GETSTATIC 全量起锚，细查 = tryMatchAt：memberOf 解析字段语义，
/// desc 钉 `[Ljava/lang/String;`（形态 1）/ `[[B`（形态 2）（knowledge
/// 表），后继非 NOP 指令逐条钉形（int 常量装载族、AALOAD、DUP、
/// ARRAYLENGTH、INVOKESTATIC desc `([BII)Ljava/lang/String;`）。跨度是**前
/// 缀钉面**，形态 1 到 aaload、形态 2 到 invokestatic 为止，后继与本站无
/// 关。NOP 跳过（squeeze 遗留落点不驻栈，number_fold 同口径）；锚点之后
/// 到尾条之间任何带标签指令 = 合并点，整站放弃。三级分工：S2 形状钉面 →
/// S3 Era 检查（Era::Kasumi 未判整站拒绝，TSV 是换代表面，Era 分支唯一合
/// 法落点）→ 旁证降级检查（TSV 未装载整站拒绝：Absent = CLI 未注入，
/// Unreadable = 文件缺失/不可读）→ S1 明文表核对（(owner, fieldKey, idx)
/// 精确命中 = 定案；无命中 = noPlaintext，dumper 只导出运行期缓存非空的
/// 槽位，覆盖是部分的，缺席不是异常，参考实现同样静默跳过）。S1 无旁证不可定
/// 案是数据流事实（明文本身就是要回填的内容）；本仓把无旁证显式计数进
/// rejects，改写面语义与参考实现逐位一致。
///
/// ## REWRITE
///
/// 按 uri 聚类 + roundTripSafe 前置检查。锚点原地换 ldc（srcPc 保留），跨度
/// 其余非 NOP 指令 eraseBatch（kRejectIfUsed：受害者被异常表引用即拒收，
/// 回滚锚点整站放弃，不留半改写态）；NOP 遗留落点不动，DeadCode 收尾。
/// CP 先 intern（Utf8 findUtf8 → miss 才 addUtf8；String 包装先线性扫复用
/// → miss 才 addString，0 = 失败哨兵）再动 DOM。明文 MUTF-8 转码失败（过
/// 短编码/挂续字节）整站拒绝，旁证是外部注入，不猜。失败粒度 = 类：帧
/// 链/编码失败整方法放弃，形态分列计数只在类写回成功后归账。
///
/// ## PROOF
///
/// pass_clock::Suspend + 类数不增 + 脏类可重新解析 + residual：改写后容器
/// 里仍全形 + 明文表命中的站点数（countResidual 与 scan 同判据重钉）。
/// 旁证缺席的形状命中不进 residual，那是覆盖面事实（运行期缓存为空的槽
/// 位 dumper 导不出来），不是还原缺陷。语料单 jar 检查要求 residual = 0。
///
/// ## 已知边界
///
/// char[][] 密文载体（`a([CII)` 解密器）不在面内，参考实现同样只处理
/// byte[][] 形态 2 与 String[] 形态 1，样本未见 char 站点。明文表是运行期
/// 快照：缓存槽为 null 的下标旁证缺席，参考实现语义 = 跳过，本仓 = noPlaintext
/// 计数。换代面（TSV 列格式 / `byte:` 前缀 / 两形态描述符都钉自当前
/// Kasumi 版本）由 Era 检查整站拒绝兜底。形态 2 的解密方法 owner 不与本站
/// getstatic owner 互钉（参考实现同口径），数据流由栈形锁定，表键只认
/// getstatic 侧 owner/field/idx。

#include "grunt/string_cache_fold.hpp"

#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/kasumi_names.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::string_cache_fold {

namespace {

namespace ir = jbc::ir;
using jbc::core::CpTag;
using jbc::core::Op;

constexpr ca::usize kMaxSpan = 7;   ///< 最长跨度（形态 2 恰 7 条：含锚点）

// ============================================================================
// ==== classfile MUTF-8 编码（明文 → 常量池 Utf8 字节面）=====================
// ============================================================================

/// @brief UTF-8 → classfile modified UTF-8（NUL 走 0xC0 0x80；增补平面码点
///        拆代理对各落 3 字节，classfile Utf8 不允许 4 字节形）。输入非合法
///        UTF-8（含挂续字节/越界形/过短编码）返回 false，调用方整站拒绝，
///        旁证内容是外部注入，不猜。
bool utf8ToMutf8(std::string_view in, std::string& out)
{
    out.clear();
    ca::usize i = 0;
    while (i < in.size()) {
        const ca::u8 b0 = static_cast<ca::u8>(in[i]);
        ca::u32     cp  = 0;
        ca::usize   adv = 0;
        if (b0 < 0x80) {
            cp  = b0;
            adv = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (in.size() - i < 2) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            if ((b1 & 0xC0) != 0x80) return false;
            cp  = (static_cast<ca::u32>(b0 & 0x1F) << 6) | (b1 & 0x3F);
            adv = 2;
            if (cp < 0x80) return false;   // 过短编码（严谨面）
        } else if ((b0 & 0xF0) == 0xE0) {
            if (in.size() - i < 3) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            const ca::u8 b2 = static_cast<ca::u8>(in[i + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            cp  = (static_cast<ca::u32>(b0 & 0x0F) << 12)
                | (static_cast<ca::u32>(b1 & 0x3F) << 6) | (b2 & 0x3F);
            adv = 3;
            if (cp < 0x800) return false;   // 过短编码（严谨面）
        } else if ((b0 & 0xF8) == 0xF0) {
            if (in.size() - i < 4) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            const ca::u8 b2 = static_cast<ca::u8>(in[i + 2]);
            const ca::u8 b3 = static_cast<ca::u8>(in[i + 3]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
                return false;
            }
            cp  = (static_cast<ca::u32>(b0 & 0x07) << 18)
                | (static_cast<ca::u32>(b1 & 0x3F) << 12)
                | (static_cast<ca::u32>(b2 & 0x3F) << 6) | (b3 & 0x3F);
            adv = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) return false;
        } else {
            return false;   // 0xF8..0xFF / 挂续字节：非合法 UTF-8
        }

        if (cp == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
        } else if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            // 增补平面 → 代理对（各按 3 字节形落池，classfile 无 4 字节形）。
            const ca::u32 v  = cp - 0x10000;
            const ca::u16 hi = static_cast<ca::u16>(0xD800 | (v >> 10));
            const ca::u16 lo = static_cast<ca::u16>(0xDC00 | (v & 0x3FF));
            const auto    emit = [&](ca::u16 u) {
                out.push_back(static_cast<char>(0xE0 | (u >> 12)));
                out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
            };
            emit(hi);
            emit(lo);
        }
        i += adv;
    }
    return true;
}

// ============================================================================
// ==== 形状钉面（S2；常量装载读数走 const_args）==============================
// ============================================================================

/// @brief 跨度内部标签检查：锚点之后到尾条之间任何指令带标签 = 合并点，整站
///        放弃（外部到达路径的栈形状不可知，不猜）。与 number_fold 同口径。
bool labelBlocked(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                  ca::usize firstPos, ca::usize lastPos)
{
    for (ca::usize p = firstPos + 1; p <= lastPos && p < order.size(); ++p) {
        if (!dom.labelsAt(order[p]).empty()) return true;
    }
    return false;
}

/// @brief 单站点形状钉面：从锚点位收最多 kMaxSpan-1 条后继非 NOP 指令（NOP
///        是 squeeze 遗留落点，不驻栈跳过），按形态逐条比对。命中时产出
///        （form, owner, fieldKey, idx）与尾条位置；不命中返回 false，形状
///        不全即非本家站点（或程序本来的数组读），静默跳过。
bool tryMatchAt(ir::ClassSession& s, const ir::CodeDom& dom,
                const std::vector<ir::InsnId>& order, ca::usize anchorPos, Site& out,
                ca::usize& lastPos)
{
    const ir::Insn* anchor = dom.peek(order[anchorPos]);
    if (anchor == nullptr || anchor->op != Op::GETSTATIC) return false;

    ir::ClassSession::MemberSem sem;
    if (!s.memberOf(*anchor, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field) {
        return false;
    }

    Form form = Form::CacheRead;
    if (sem.desc == knowledge::kKasumiCacheFieldDesc) {
        form = Form::CacheRead;
    } else if (sem.desc == knowledge::kKasumiCryptFieldDesc) {
        form = Form::DirectDecrypt;
    } else {
        return false;   // 不是本家字段描述符：普通 getstatic，静默跳过
    }

    // 收锚点之后最多 kMaxSpan-1 条非 NOP 指令。
    std::vector<ca::usize>       pos;
    std::vector<const ir::Insn*> seq;
    for (ca::usize p = anchorPos + 1; p < order.size() && seq.size() < kMaxSpan - 1; ++p) {
        const ir::Insn* cur = dom.peek(order[p]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        pos.push_back(p);
        seq.push_back(cur);
    }

    ca::i32 idx = 0;
    if (seq.size() < 2 || !const_args::decodeConstInt(s, *seq[0], idx) || seq[1]->op != Op::AALOAD) {
        return false;
    }
    if (form == Form::CacheRead) {
        // 形态 1：锚 + idx + aaload（跨度是前缀钉面，aaload 之后是什么指令
        // 与本站无关，参考实现 nextRealAfter 同口径）。
        out.owner    = std::string(sem.owner);
        out.fieldKey = std::string(sem.name);
        out.index    = idx;
        out.form     = form;
        lastPos      = pos[1];
        return true;
    }

    // 形态 2：锚 + idx + aaload + dup + arraylength + off + invokestatic
    //（同为前缀钉面，invokestatic 之后与本站无关）。
    if (seq.size() < 6) return false;
    if (seq[2]->op != Op::DUP || seq[3]->op != Op::ARRAYLENGTH) return false;
    ca::i32 off = 0;
    if (!const_args::decodeConstInt(s, *seq[4], off)) return false;
    const ir::Insn* invoke = seq[5];
    if (invoke->op != Op::INVOKESTATIC) return false;
    ir::ClassSession::MemberSem msem;
    if (!s.memberOf(*invoke, msem)
        || msem.kind != ir::ClassSession::MemberSem::Kind::Method
        || msem.desc != knowledge::kKasumiByteDecryptDesc) {
        return false;
    }
    // 方法名不钉（样本相关）；明文表 key 用 byte: 前缀 + getstatic 字段名
    // （参考实现口径：表键与解密方法名无关，栈形已保证 invoke 消费的正是本
    // getstatic 装载的密文行）。
    out.owner    = std::string(sem.owner);
    out.fieldKey = std::string(knowledge::kKasumiByteKeyPrefix) + std::string(sem.name);
    out.index    = idx;
    out.form     = form;
    lastPos      = pos[5];
    return true;
}

/// @brief 明文表核对（S1）：命中产出含明文的完整 Site；无命中返回 false
///        （noPlaintext，参考实现静默跳过对齐，旁证覆盖是部分的，缺席不是异常）。
bool lookupPlaintext(const PlaintextTable& table, const Site& shape, Site& out)
{
    const auto oit = table.find(shape.owner);
    if (oit == table.end()) return false;
    const auto fit = oit->second.find(shape.fieldKey);
    if (fit == oit->second.end()) return false;
    const auto iit = fit->second.find(shape.index);
    if (iit == fit->second.end()) return false;

    out           = shape;
    out.plaintext = iit->second;
    return true;
}

/// @brief 单方法 SCAN。逐 GETSTATIC 起锚：形状钉面 → Era 检查（S3）→ 旁证降级
///        检查 → 明文表核对（S1）。
void scanMethod(ir::ClassSession& s, const std::string& uri, ca::usize methodIndex,
                const ScanInput& input, ScanResult& result)
{
    if (!ir::hasCode(s, methodIndex)) return;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      pos   = 0;
    while (pos < order.size()) {
        const ir::Insn* in = cu->dom.peek(order[pos]);
        if (in == nullptr || in->op != Op::GETSTATIC) {
            ++pos;
            continue;
        }

        Site      shape;
        ca::usize lastPos = 0;
        if (!tryMatchAt(s, cu->dom, order, pos, shape, lastPos)
            || labelBlocked(cu->dom, order, pos, lastPos)) {
            ++pos;   // 形状不全 / 合并点：非本家站点，静默跳过
            continue;
        }
        ++result.totalShapeHits;
        pos = lastPos + 1;   ///< 跨度内部不再起锚（站点跨度不重叠）

        // ---- S3 检查：TSV 旁证属换代表面，Era 分支是它唯一合法落点 ----
        if (input.era != knowledge::Era::Kasumi) {
            Reject rej;
            rej.uri         = uri;
            rej.methodIndex = methodIndex;
            rej.anchorSrcPc = in->srcPc;
            rej.why         = "Era 未判定：明文旁证表不可用（fail-closed）";
            result.rejects.push_back(std::move(rej));
            ++result.eraBlocked;
            continue;
        }
        // ---- 旁证降级面：Absent / Unreadable 全拒（参考实现对齐：无旁证不改写；
        //      本仓把无旁证显式计数）----
        if (input.tsvState != TsvState::Loaded) {
            Reject rej;
            rej.uri         = uri;
            rej.methodIndex = methodIndex;
            rej.anchorSrcPc = in->srcPc;
            rej.why         = input.tsvState == TsvState::Unreadable
                                  ? "TSV 旁证文件不可读：旁证降级（fail-closed）"
                                  : "TSV 旁证未注入：无任何旁证（fail-closed）";
            result.rejects.push_back(std::move(rej));
            ++result.tsvBlocked;
            continue;
        }
        // ---- S1：明文表核对定案；无命中不猜（参考实现静默跳过对齐）----
        Site site;
        if (!lookupPlaintext(input.plaintexts, shape, site)) {
            ++result.noPlaintext;
            continue;
        }
        site.uri         = uri;
        site.methodIndex = methodIndex;
        site.anchorSrcPc = in->srcPc;
        result.sites.push_back(std::move(site));
    }
}

// ============================================================================
// ==== 改写 ==================================================================
// ============================================================================

/// @brief 明文 intern 进常量池：Utf8（命中既有池不膨胀）+ CONSTANT_String
///        包装（线性扫既有 String 条目复用）。CP 先行，失败面在动 DOM 之前
///        收口。@return String 条目索引；0 = 失败（占位槽索引天然是哨兵）。
ca::u32 internStringConst(jbc::core::ConstantPool& cp, const std::string& mutf8)
{
    ca::u32 utf8Idx = cp.findUtf8(mutf8);
    if (utf8Idx == 0) {
        utf8Idx = cp.addUtf8(mutf8);
        if (utf8Idx == 0) return 0;
    }
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        const jbc::core::ConstantPool::Entry& e = cp.at(i);
        if (e.tag == CpTag::String && e.a == utf8Idx) return i;
    }
    return cp.addString(utf8Idx);
}

/// @brief 单站点改写。@return 命中形态；false = 未动（failed 计数）。
///
/// 重定位 + 复核（scan 之后字节可能已被同方法前站改写，不盲改）：锚点
/// srcPc+op 重定位、形状重钉、标签检查复核。锚点原地换 `ldc <明文>`（srcPc
/// 保留），跨度其余非 NOP 指令擦除（kRejectIfUsed 拒收即回滚锚点，不留
/// 半改写态）。
bool rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu, const Site& site)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();

    // 重定位：srcPc + op 跨会话稳定（InsnId 不带出去）。
    ca::usize anchorPos = static_cast<ca::usize>(-1);
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const ir::Insn* in = cu.dom.peek(order[pos]);
        if (in != nullptr && in->srcPc == site.anchorSrcPc && in->op == Op::GETSTATIC) {
            anchorPos = pos;
            break;
        }
    }
    if (anchorPos == static_cast<ca::usize>(-1)) return false;

    // 形状复核：scan 之后字节可能已被前站改写，重钉一次并核对身份。
    Site      shape;
    ca::usize lastPos = 0;
    if (!tryMatchAt(s, cu.dom, order, anchorPos, shape, lastPos) || shape.form != site.form
        || shape.owner != site.owner || shape.fieldKey != site.fieldKey
        || shape.index != site.index || labelBlocked(cu.dom, order, anchorPos, lastPos)) {
        return false;
    }

    // 明文 MUTF-8 编码（旁证是外部注入，编码失败不猜）。
    std::string mutf8;
    if (site.plaintext.empty() || !utf8ToMutf8(site.plaintext, mutf8)) return false;

    // CP 先 intern：失败面在动 DOM 之前收口。
    const ca::u32 strIdx = internStringConst(s.backing().cp, mutf8);
    if (strIdx == 0) return false;

    // 锚点原地换 ldc（srcPc 保留：pcmap/调试行落点不变）。
    auto slotR = cu.dom.get(order[anchorPos]);
    if (slotR.is_err()) return false;
    ir::Insn*     anchor   = slotR.unwrap();
    const ir::Insn original = *anchor;

    ir::Insn fresh = ir::CodeEditor::loadConst(strIdx);
    fresh.srcPc    = original.srcPc;
    *anchor        = fresh;

    // 跨度其余非 NOP 指令擦除。eraseBatch kRejectIfUsed：受害者被异常表引用
    // 时拒收，回滚锚点整站放弃，不留常量已覆盖、跨度还在的半改写态。
    std::vector<ir::InsnId> victims;
    for (ca::usize p = anchorPos + 1; p <= lastPos && p < order.size(); ++p) {
        const ir::Insn* victim = cu.dom.peek(order[p]);
        if (victim == nullptr || victim->op == Op::NOP) continue;
        victims.push_back(order[p]);
    }
    if (!victims.empty() && cu.dom.eraseBatch(victims).is_err()) {
        *anchor = original;
        return false;
    }
    return true;
}

/// @brief 单方法 REWRITE 驱动。@return 命中站点数（命中形态落 hits，类级
///        放弃不许虚报站点，调用方在类写回成功后归账）。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit& cu,
                        const std::vector<const Site*>& sites, std::vector<Form>& hits,
                        ca::usize& failed)
{
    ca::usize hit = 0;
    for (const Site* site : sites) {
        if (rewriteSite(s, cu, *site)) {
            hits.push_back(site->form);
            ++hit;
        } else {
            ++failed;
        }
    }
    return hit;
}

// ============================================================================
// ==== PROOF：residual 统计 ==================================================
// ============================================================================

/// @brief 容器里仍全形 + 明文表命中的站点数（residual 口径）。
///
/// 形状钉面是容器自证事实（与 scan 同判据），表命中保证 residual 只数
/// 本可折叠而未折叠的站点，旁证缺席的形状命中不进 residual（那是
/// 覆盖面事实，不是还原缺陷）。
ca::usize countResidual(const mjbase::model::ResourceContainer& container,
                        const PlaintextTable& table)
{
    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            const std::vector<ir::InsnId>& order = cu->dom.order();
            ca::usize                      pos   = 0;
            while (pos < order.size()) {
                const ir::Insn* in = cu->dom.peek(order[pos]);
                if (in == nullptr || in->op != Op::GETSTATIC) {
                    ++pos;
                    continue;
                }
                Site      shape;
                ca::usize lastPos = 0;
                if (!tryMatchAt(s, cu->dom, order, pos, shape, lastPos)
                    || labelBlocked(cu->dom, order, pos, lastPos)) {
                    ++pos;
                    continue;
                }
                Site hit;
                if (lookupPlaintext(table, shape, hit)) ++residual;
                pos = lastPos + 1;
            }
        }
    });
    return residual;
}

}   // namespace

// ============================================================================
// ==== TSV 旁证解析（公开面：adapter 调用注入，unittest 直测）================
// ============================================================================

std::string unescapeTsv(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (ca::usize i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '\\': out.push_back('\\'); break;
            case 't': out.push_back('\t'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            default: out.push_back(s[i + 1]); break;
            }
            ++i;
        }
        else {
            out.push_back(s[i]);
        }
    }
    return out;
}

namespace {

/// @brief 严格十进制 int 解析（参考实现 std::stoi 的 fail-closed 替身：越界/
///        尾随垃圾都拒，不猜）。
bool parseIndex(std::string_view s, ca::i32& out)
{
    if (s.empty()) return false;
    ca::usize i  = 0;
    bool      neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i   = 1;
        if (s.size() == 1) return false;
    }
    ca::i64 acc = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        acc = acc * 10 + (s[i] - '0');
        if (acc > 2147483648LL) return false;   // INT_MIN 需要的量级之上即拒
    }
    if (neg) acc = -acc;
    if (acc > 2147483647LL || acc < -2147483648LL) return false;
    out = static_cast<ca::i32>(acc);
    return true;
}

}   // namespace

ca::usize parseTsv(std::string_view content, PlaintextTable& out)
{
    ca::usize rows = 0;
    ca::usize pos  = 0;
    while (pos < content.size()) {
        const ca::usize eol = content.find('\n', pos);
        const std::string_view line =
            content.substr(pos, eol == std::string_view::npos
                                    ? std::string_view::npos
                                    : eol - pos);
        pos = eol == std::string_view::npos ? content.size() : eol + 1;

        std::string_view t = line;
        if (!t.empty() && t.back() == '\r') t.remove_suffix(1);   // 保守吃 CR
        if (t.empty()) continue;

        // 按 \t 分 4 列：plaintext 内的真实 \t 已被 dumper 转义（\t → "\\t"），
        // 前三个 \t 恰是列分隔（参考实现 find 三连同口径）。
        const ca::usize a = t.find('\t');
        if (a == std::string_view::npos) continue;
        const ca::usize b = t.find('\t', a + 1);
        if (b == std::string_view::npos) continue;
        const ca::usize c = t.find('\t', b + 1);
        if (c == std::string_view::npos) continue;

        std::string owner(t.substr(0, a));
        const std::string field(t.substr(a + 1, b - a - 1));
        ca::i32 idx = 0;
        if (!parseIndex(t.substr(b + 1, c - b - 1), idx)) continue;
        std::string plain = unescapeTsv(t.substr(c + 1));

        // owner 在 TSV 里是点分（dev.kasumi.obfuscator.w），转 internal slash。
        for (char& ch : owner) {
            if (ch == '.') ch = '/';
        }
        out[std::move(owner)][std::move(field)].emplace(idx, std::move(plain));
        ++rows;
    }
    return rows;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    ScanResult result;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto         session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            scanMethod(s, uri, mi, input, result);
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    // 站点按 uri 聚类（对齐 number_fold：万类大 jar 上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit  = 0;
        std::vector<Form>   forms;
    };
    std::vector<PendingWrite> pending;

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

        // 前置检查：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize         hit    = 0;
        ca::usize         failed = 0;
        std::vector<Form> forms;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize         methodHit    = 0;
            ca::usize         methodFailed = 0;
            std::vector<Form> methodForms;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    methodHit = rewriteMethod(s, cu, sitesOfMethod, methodForms,
                                              methodFailed);
                    return methodHit > 0;
                });
            if (!ok) {
                // 帧链/编码失败：该方法整方法放弃（session 内半改写态不外泄）。
                failed += methodHit + methodFailed;
                continue;
            }
            hit += methodHit;
            failed += methodFailed;
            forms.insert(forms.end(), methodForms.begin(), methodForms.end());
        }
        dirty.failed += failed;
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit,
                                       std::move(forms)});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.hit;
        dirty.classes += 1;
        for (const Form form : item.forms) {
            if (form == Form::CacheRead) {
                ++dirty.rewrittenCacheReads;
            } else {
                ++dirty.rewrittenDirectDecrypt;
            }
        }
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore, const ScanInput& input)
{
    // proof 是效力检查（不变量复核），不许被 pass 级时间片截短，挂起期间
    // forEachClass 全量放行（residual 统计同样要看到全量类）。
    pass_clock::Suspend                  suspend;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    result.residual = countResidual(container, input.plaintexts);
    return result;
}

}   // namespace grunt::string_cache_fold
