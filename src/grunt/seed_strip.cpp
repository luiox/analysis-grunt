/// @file        seed_strip.cpp
/// @brief       残留 `seed_strip` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 栈深模拟
///
/// `spanDepthDelta` 词汇表逐指令增量；组从 `GETSTATIC F`（+1）起，到
/// `IASTORE`（-3：数组引用 + 下标 + 值出栈）归零收尾。嵌套 `GETSTATIC F`
/// （值链里再读同一数组，grunt 用前一元素算下一元素）深度 +1，允许嵌套。
///
/// ## 复核纪律
///
/// rewrite 对每个字段**重新收集跨度再擦**：同 clinit 里多个种子字段时，
/// 先擦的字段改变布局，后擦的字段必须按新 DOM 重推。逐字段全有或全无：
/// 任一跨度擦除失败即放弃该字段（已擦的跨度语义仍安全，普查检查保证无类
/// 外读，残余读检查收尾）。
///
/// ## 残余读检查
///
/// 擦完逐条扫 clinit：该字段的 `GETSTATIC` 必须清零，否则视为形状误判，
/// 放弃该字段（防收集器把非种子读吸进跨度）。
#include "grunt/seed_strip.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/field_census.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::seed_strip {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic = 0x0008;

/// @brief 种子跨度词汇表的栈深增量；不在词汇表返回最小值（调用方放弃）。
///
/// 覆盖：常量装载 / 栈整形 / 整长算术与转换（除法族可能抛算术异常，排除）。
/// 局部变量读写不进词汇表（跨度内写局部、跨度外读会破语义）。
int spanDepthDelta(Op op)
{
    const int o = static_cast<int>(op);
    if (o == 0x00) return 0;                        // NOP
    if (o >= 0x02 && o <= 0x08) return 1;           // ICONST
    if (o == 0x09 || o == 0x0a) return 2;           // LCONST
    if (o >= 0x0b && o <= 0x0d) return 1;           // FCONST
    if (o == 0x0e || o == 0x0f) return 2;           // DCONST
    if (o == 0x10 || o == 0x11) return 1;           // BIPUSH/SIPUSH
    if (o == 0x57) return -1;                       // POP
    if (o == 0x58) return -2;                       // POP2
    if (o >= 0x59 && o <= 0x5b) return 1;           // DUP 家族
    if (o >= 0x5c && o <= 0x5e) return 2;           // DUP2 家族
    if (o == 0x5f) return 0;                        // SWAP
    if (o >= 0x74 && o <= 0x77) return 0;           // *NEG
    if (o >= 0x78 && o <= 0x7a) return -1;          // ISHL/ISHR/IUSHR
    if (o >= 0x7b && o <= 0x7d) return -1;          // LSHL/LSHR/LUSHR
    if (o == 0x7e || o == 0x80 || o == 0x82) return -1;   // IAND/IOR/IXOR
    if (o == 0x7f || o == 0x81 || o == 0x83) return -2;   // LAND/LOR/LXOR
    if ((o >= 0x60 && o <= 0x6b) || o == 0x6e || o == 0x6f) {
        return (o & 1) != 0 ? -2 : -1;              // 加减乘族（奇数=long/double）
    }
    if (o == 0x6c || o == 0x70 || o == 0x6d || o == 0x71) {
        return -128;                                // IDIV/IREM/LDIV/LREM 排除
    }
    if (o == 0x85 || o == 0x87 || o == 0x8c || o == 0x8d) return 1;    // I2L/I2D/F2L/F2D
    if (o == 0x88 || o == 0x89 || o == 0x8e || o == 0x8f) return -1;   // L2I/L2F/D2I/D2L
    if ((o >= 0x86 && o <= 0x8b && o != 0x87 && o != 0x88 && o != 0x89)
        || (o >= 0x90 && o <= 0x93)) {
        return 0;                                   // 同宽转换
    }
    if (o >= 0x94 && o <= 0x98) return -1;          // LCMP/FCMP 族
    return -128;
}

/// @brief 收集一个字段的建表跨度（全有或全无）。成功返回 true，spans 填充。
///
/// 头 = NEWARRAY + PUTSTATIC F；组 = GETSTATIC F 起、栈深模拟、IASTORE 归零
/// 收尾。NOP 是此前改写的遗留落点，跳过。任何词汇表外指令 / 标签 / 形状不
/// 符即整字段失败（spansOk=false）。
bool collectSpans(ir::ClassSession& s, ir::CodeDom& cdom, const std::string& owner,
                  const std::string& fname,
                  std::vector<std::vector<ir::InsnId>>& spans)
{
    const std::vector<ir::InsnId>& corder = cdom.order();
    ca::usize                      pos    = 0;
    bool                           headSeen = false;

    while (pos < corder.size()) {
        const ir::Insn* n = cdom.peek(corder[pos]);
        if (n == nullptr) return false;
        if (n->op == Op::NOP) {
            ++pos;
            continue;
        }
        if (n->op == Op::NEWARRAY) {
            // 头：NEWARRAY 后第一个非 NOP 必须是 PUTSTATIC F。
            std::vector<ir::InsnId> head{corder[pos]};
            ca::usize               q = pos + 1;
            bool                    headOk = false;
            while (q < corder.size()) {
                const ir::Insn* m = cdom.peek(corder[q]);
                if (m == nullptr) return false;
                if (m->op == Op::NOP) {
                    ++q;
                    continue;
                }
                if (m->op != Op::PUTSTATIC) break;
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*m, sem) || sem.owner != owner || sem.name != fname) break;
                head.push_back(corder[q]);
                headOk = true;
                pos    = q + 1;
                break;
            }
            if (!headOk) {
                ++pos;
                continue;
            }
            spans.push_back(std::move(head));
            headSeen = true;
            continue;
        }
        if (n->op == Op::GETSTATIC) {
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*n, sem) || sem.owner != owner || sem.name != fname) {
                ++pos;
                continue;
            }
            if (!headSeen) return false;   // 读无建表：不是本工具改出的形态
            std::vector<ir::InsnId> group{corder[pos]};
            int                     depth = 1;
            ca::usize               q     = pos + 1;
            bool                    closed = false;
            while (depth > 0 && q < corder.size()) {
                const ir::Insn* m = cdom.peek(corder[q]);
                if (m == nullptr) return false;
                if (m->op == Op::NOP) {
                    ++q;
                    continue;
                }
                group.push_back(corder[q]);
                if (m->op == Op::GETSTATIC) {
                    ir::ClassSession::MemberSem gsem;
                    if (!s.memberOf(*m, gsem) || gsem.owner != owner
                        || gsem.name != fname) {
                        return false;
                    }
                    depth += 1;
                } else if (m->op == Op::IASTORE) {
                    depth -= 3;
                } else if (m->op == Op::LDC || m->op == Op::LDC_W
                           || m->op == Op::LDC2_W) {
                    // 组内只允许 Int/Float 字面量（Long/Double 不是 grunt 形态）。
                    ir::ClassSession::ConstSem csem;
                    using K = ir::ClassSession::ConstSem::Kind;
                    if (!s.constOf(*m, csem)
                        || (csem.kind != K::Int && csem.kind != K::Float)) {
                        return false;
                    }
                    depth += 1;
                } else {
                    const int delta = spanDepthDelta(m->op);
                    if (delta == -128) return false;
                    depth += delta;
                }
                if (depth < 0) return false;
                if (depth == 0) {
                    if (m->op != Op::IASTORE) return false;
                    closed = true;
                }
                ++q;
            }
            if (!closed || depth != 0) return false;
            spans.push_back(std::move(group));
            pos = q;
            continue;
        }
        ++pos;
    }
    return !spans.empty();
}

/// @brief 擦除一个连续跨度（首条 NOP 保标签锚，其余 eraseBatch）；中段有
/// 标签即放弃。
bool eraseSpan(ir::CodeDom& dom, const std::vector<ir::InsnId>& span)
{
    if (span.empty()) return true;
    for (ca::usize i = 1; i < span.size(); ++i) {
        if (!dom.labelsAt(span[i]).empty()) return false;
    }
    {
        auto mr = dom.get(span.front());
        if (mr.is_err()) return false;
        ir::Insn*     anchor  = mr.unwrap();
        const ca::u32 keepPc  = anchor->srcPc;
        *anchor               = ir::Insn{};
        anchor->op            = Op::NOP;
        anchor->srcPc         = keepPc;
    }
    if (span.size() == 1) return true;
    return dom.eraseBatch({span.begin() + 1, span.end()}).is_ok();
}

/// @brief clinit 下标；无则 npos。
ca::usize findClinit(ir::ClassSession& s)
{
    const ir::ClassUnit& unit = s.unit();
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (unit.symbols().str(unit.methods[mi].name) == "<clinit>" && ir::hasCode(s, mi)) {
            return mi;
        }
    }
    return static_cast<ca::usize>(-1);
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    // 访问面普查不完整（会话打不开的类 / promote 失败的方法）＝引用证据不足
    // ，写读全在本类 clinit判据会漏计漏普查面上的读，拆表即运行期断供，
    // fail-closed 整轮放弃（与 dead_member 引用普查同一课，宁误留不误删）。
    field_census::Census     census;
    const field_census::Stat stat = field_census::scanFieldAccess(container, census);
    result.verifierDead = stat.methodsVerifierDead;
    if (!stat.complete()) {
        result.censusIncomplete = true;
        std::fprintf(stderr,
                     "[grunt.SeedStrip] 普查不完整（类 %zu / 方法 %zu 未普查），整轮放弃\n",
                     stat.classesSkipped, stat.methodsPromoteFailed);
        return result;
    }

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession&    s      = *session;
        jbc::core::ClassFile& cf    = s.backing();
        const std::string    owner(cf.thisName());
        const ca::usize      clinit = findClinit(s);
        if (clinit == static_cast<ca::usize>(-1)) return;

        for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
            const jbc::core::MemberInfo& fld = cf.fields[fi];
            if ((fld.access & kAccStatic) == 0) continue;
            const std::string fname(cf.utf8(fld.nameCp));
            if (cf.utf8(fld.descCp) != "[I") continue;

            // 访问面检查：写全在本类 clinit，读也全在本类 clinit。
            const auto acc = census.find(owner + "::" + fname);
            if (acc == census.end()) continue;
            bool ok = true;
            bool anyWrite = false;
            for (const field_census::FieldAcc& a : acc->second) {
                if (a.uri != uri || !a.inClinit) ok = false;
                if (a.write) anyWrite = true;
            }
            if (!ok || !anyWrite) continue;

            // 建表形状检查。
            auto promoted = ir::promoteMethod(s, clinit);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            std::vector<std::vector<ir::InsnId>> spans;
            if (!collectSpans(s, cu->dom, owner, fname, spans)) continue;

            Site site;
            site.uri       = uri;
            site.fieldName = fname;
            site.spanCount = spans.size();
            result.sites.push_back(std::move(site));
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

    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           fields = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;

        std::vector<const Site*> sitesOf;
        for (const Site& site : result.sites) {
            if (site.uri == uri) sitesOf.push_back(&site);
        }

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;
        const std::string owner(s.backing().thisName());
        const ca::usize   clinit = findClinit(s);
        if (clinit == static_cast<ca::usize>(-1)) return;

        ca::usize hit = 0;
        const bool ok = rewrite_tools::mutateMethodWithFrames(
            s, clinit, frames.resolver(), [&](ir::CodeUnit& cu) {
                ir::CodeDom& cdom = cu.dom;

                for (const Site* site : sitesOf) {
                    ++dirty.attempted;

                    // 复核：访问面与建表形状（scan 之后容器未变，形状必须重推
                    // ，同方法其他字段的跨度可能改变布局）。
                    std::vector<std::vector<ir::InsnId>> spans;
                    if (!collectSpans(s, cdom, owner, site->fieldName, spans)) {
                        ++dirty.failed;
                        continue;
                    }

                    // 逐跨度擦除；任一失败即放弃该字段（不落半截）。
                    bool spanOk = true;
                    for (const auto& span : spans) {
                        if (!eraseSpan(cdom, span)) spanOk = false;
                    }
                    if (!spanOk) {
                        ++dirty.failed;
                        continue;
                    }

                    // 残余读检查：clinit 里该字段的 GETSTATIC 必须清零。
                    bool residual = false;
                    for (const ir::InsnId id : cdom.order()) {
                        const ir::Insn* n = cdom.peek(id);
                        if (n == nullptr || n->op != Op::GETSTATIC) continue;
                        ir::ClassSession::MemberSem sem;
                        if (s.memberOf(*n, sem) && sem.owner == owner
                            && sem.name == site->fieldName) {
                            residual = true;
                            break;
                        }
                    }
                    if (residual) {
                        ++dirty.failed;
                        continue;
                    }
                    ++hit;
                }
                return hit > 0;
            });
        if (!ok) return;   // fail-closed：clinit 放弃，本类不写 pending
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.fields += item.fields;
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

}   // namespace grunt::seed_strip
