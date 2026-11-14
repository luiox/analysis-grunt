/// @file        pool_array_fold.cpp
/// @brief       残留 `pool_array_fold` 三段实现：SCAN / REWRITE / PROOF。
///
/// SCAN 三遍式：
///   1. 池字段普查：static `String[]` 字段 → 候选组 (owner, field)；同遍
///      写面普查——非 `<clinit>` 方法对池字段的 PUTSTATIC，或 GETSTATIC +
///      AASTORE 同方法共存（数组引用可能逃逸改写，无法排除就整组放弃）；
///   2. clinit 求值：SimExecutor 编排层（str/num 扩展 + scan_toolbox 的池
///      mock），多轮接力（PUTSTATIC 棒传，依赖别的池类先收敛），读回会话
///      静态区的数组终态——Str 载荷与 `new String(char[])` 的 Obj 载荷两落
///      地形态都收（dead_member 池明文提取同口径）；Bail / 熔断 = 该组放弃；
///   3. 读点三元组：全类方法扫 `GETSTATIC <本类池>; <int idx>; AALOAD`（NOP
///      跳过、跨度无合并点标签），终态表命中 → Site，孤立代理明文拒。
///
/// REWRITE 对齐 string_pool_fold：roundTripSafe 前置 + FrameContext Strict
/// 帧链 + 锚点原地 ldc（srcPc 保留）+ eraseBatch（kRejectIfUsed 拒收回滚）。
///
/// PROOF：类数不增 + 脏类重解析 + residual（重跑求值与三元组 countOnly）。
#include "grunt/pool_array_fold.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/junk_region_strip.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/pass_clock.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/engine.hpp"
#include "jsim/num.hpp"
#include "jsim/str.hpp"
#include "jsim/value.hpp"

namespace grunt::pool_array_fold {

namespace {

namespace ir  = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic = 0x0008;

/// @brief 求值终态表：(owner, field) → 下标 → 明文码元。
using PlainTable = std::unordered_map<std::string, std::unordered_map<ca::i32, PlainUnits>>;

std::string poolKey(const std::string& owner, const std::string& field)
{
    return owner + "::" + field + "::" + std::string(knowledge::kGruntPoolFieldDesc);
}

/// @brief 一个候选池组（owner 类 + 字段名 + 求值/写面状态）。
struct PoolGroup
{
    std::string owner;
    std::string field;
    bool        writeSuspect = false;   ///< 写面普查不过（整组放弃）
    bool        evalOk       = false;   ///< clinit 求值收敛
};

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// @brief 池字段普查 + 写面普查。@return 候选组（写嫌疑组带 writeSuspect）。
std::vector<PoolGroup> collectPools(const mjbase::model::ResourceContainer& container,
                                    ScanResult& result)
{
    std::vector<PoolGroup> groups;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession& s            = *session;
        const std::string selfName(s.unit().symbols().str(s.unit().thisClass));

        // 本类 static String[] 字段 → 候选组。
        std::vector<std::string> fields;
        const jbc::core::ClassFile& cf = s.backing();
        for (const jbc::core::MemberInfo& f : cf.fields) {
            if ((f.access & kAccStatic) == 0) continue;
            if (std::string(cf.utf8(f.descCp)) != knowledge::kGruntPoolFieldDesc) continue;
            fields.push_back(std::string(cf.utf8(f.nameCp)));
        }
        if (fields.empty()) return;

        // clinit 调用闭包（本类 invokestatic 边）：填池链 = clinit 直达/接力
        // 的方法，对池字段的写是初始化路径的一部分，不构成运行期改写嫌疑
        // （样本 C 的填池方法独立成体——K.b() 由 clinit 调用，GETSTATIC +
        // AASTORE 是它的合法填池形）。闭包识别不了的方法按嫌疑处理
        // （fail-closed 方向）。
        std::set<ca::usize> fillChain;
        {
            ca::usize clinitIdx = static_cast<ca::usize>(-1);
            for (ca::usize mi = 0; mi < s.unit().methods.size(); ++mi) {
                if (s.unit().symbols().str(s.unit().methods[mi].name) == "<clinit>") {
                    clinitIdx = mi;
                    break;
                }
            }
            if (clinitIdx != static_cast<ca::usize>(-1) && ir::hasCode(s, clinitIdx)) {
                std::vector<ca::usize> work{clinitIdx};
                fillChain.insert(clinitIdx);
                while (!work.empty()) {
                    const ca::usize cur = work.back();
                    work.pop_back();
                    auto promoted = ir::promoteMethod(s, cur);
                    if (promoted.is_err()) continue;
                    ir::CodeUnit* cu = promoted.unwrap().get();
                    if (cu == nullptr) continue;
                    for (const ir::InsnId id : cu->dom.order()) {
                        const ir::Insn* in = cu->dom.peek(id);
                        if (in == nullptr || in->op != Op::INVOKESTATIC) continue;
                        ir::ClassSession::MemberSem sem;
                        if (!s.memberOf(*in, sem)
                            || sem.kind != ir::ClassSession::MemberSem::Kind::Method
                            || sem.owner != selfName) continue;
                        for (ca::usize tj = 0; tj < s.unit().methods.size(); ++tj) {
                            const ir::MethodUnit& tm = s.unit().methods[tj];
                            if (s.unit().symbols().str(tm.name) != sem.name
                                || s.unit().symbols().str(tm.desc) != sem.desc) continue;
                            if (fillChain.insert(tj).second) work.push_back(tj);
                            break;
                        }
                    }
                }
            }
        }

        // 写面普查：clinit 闭包之外的方法对池字段的写 / 读-改写同现。
        struct WriteHit
        {
            bool putstatic = false;
            bool loadAstore = false;
        };
        std::unordered_map<std::string, WriteHit> writes;
        for (ca::usize mi = 0; mi < s.unit().methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            const ir::MethodUnit& mu = s.unit().methods[mi];
            const bool isClinit = s.unit().symbols().str(mu.name) == "<clinit>";
            const bool onChain  = fillChain.count(mi) != 0;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            // 单方法先全量收集两个标志（GETSTATIC 与 AASTORE 共存 = 数组
            // 引用可能逃逸改写，与指令先后顺序无关），方法收尾统一判定。
            std::unordered_map<std::string, bool> poolReads;
            bool astored = false;
            for (const ir::InsnId id : cu->dom.order()) {
                const ir::Insn* in = cu->dom.peek(id);
                if (in == nullptr) continue;
                if (in->op == Op::AASTORE) {
                    astored = true;
                    continue;
                }
                if (in->op != Op::GETSTATIC && in->op != Op::PUTSTATIC) continue;
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*in, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field
                    || sem.owner != selfName
                    || sem.desc != knowledge::kGruntPoolFieldDesc) continue;
                if (fields.end() == std::find(fields.begin(), fields.end(), sem.name)) continue;
                if (in->op == Op::GETSTATIC) poolReads[std::string(sem.name)] = true;
                if (in->op == Op::PUTSTATIC && !isClinit && !onChain) {
                    writes[std::string(sem.name)].putstatic = true;
                }
            }
            if (!astored) continue;
            for (const auto& [name, read] : poolReads) {
                if (read && !isClinit && !onChain) {
                    writes[name].loadAstore = true;
                }
            }
        }

        for (const std::string& f : fields) {
            PoolGroup g;
            g.owner = selfName;
            g.field = f;
            const auto it = writes.find(f);
            if (it != writes.end() && (it->second.putstatic || it->second.loadAstore)) {
                g.writeSuspect = true;
                ++result.writeSuspect;
            }
            groups.push_back(std::move(g));
        }
        ++result.poolClasses;
    });
    return groups;
}

/// @brief clinit 求值：多轮接力读回数组终态。Bail 组不入表（不猜）。
///
/// 求值净化视图：帧锚死区段保留的段尾 `athrow` 是 JVM 校验器的状态终止符
/// （上游 JunkRegionStrip 按段保留），但 libjsim 桥对含 `athrow` 的方法整
/// 方法不桥——解密器在桥面被拒，clinit 接力求值全线 Bail（真实样本实证
/// eval-bailed=16）。求值走**副本容器**：副本里把 JunkRegionStrip 的死区
/// 段与保留段尾一并 NOP（副本只喂 jsim 解释器，不做 JVM 校验，帧锚泄漏
/// 无校验面）；可达指令语义不变，运行期算什么明文还是什么。真容器不动。
void evaluatePools(const mjbase::model::ResourceContainer& container,
                   const junk_region_strip::ScanResult& junk,
                   std::vector<PoolGroup>& groups, PlainTable& table, ScanResult& result)
{
    static const bool evalTrace = std::getenv("GRUNT_POOL_EVAL_TRACE") != nullptr;

    // 求值副本：全部类条目拷贝字节，再按 JunkRegionStrip 的死区计划 NOP 化
    // （含保留段尾 athrow）。段坐标是 Code 内 code 数组偏移，必须走
    // 解析 → decodeAttr → 原位 NOP → writeClass（直改文件字节会打到类头）。
    mjbase::model::ResourceContainer evalView;
    {
        std::unordered_map<std::string, std::vector<const junk_region_strip::JunkSite*>>
            byUri;
        for (const junk_region_strip::JunkSite& site : junk.sites) {
            byUri[site.uri].push_back(&site);
        }
        container.visitResources([&](std::string_view uri, const mjbase::model::IResource& res) {
            if (res.getType() != mjbase::model::ResourceType::Class) return;
            const auto* cls = ca::core::tag_cast::dyn_cast<
                const mjbase::model::ClassResource>(&res);
            if (cls == nullptr) return;
            const auto it = byUri.find(std::string(uri));
            if (it == byUri.end()) return;
            auto session = openSession(cls->get());
            if (!session) return;
            jbc::core::ClassFile& cf = session->backing();
            bool changed = false;
            for (const junk_region_strip::JunkSite* site : it->second) {
                if (site->methodIndex >= cf.methods.size()) continue;
                jbc::core::MemberInfo& m = cf.methods[site->methodIndex];
                if (site->codeSlot >= m.attrs.size()) continue;
                auto d = cf.decodeAttr(m.attrs[site->codeSlot]);
                if (d.is_err()) continue;
                auto* bodyPtr = m.attrs[site->codeSlot].mutate();
                if (bodyPtr == nullptr) continue;
                auto& body = static_cast<jbc::core::CodeAttr&>(*bodyPtr);
                ca::u32 touched = 0;
                for (const auto& [start, len] : site->ranges) {
                    if (static_cast<ca::usize>(start) + len > body.code.size()) continue;
                    for (ca::u32 i = 0; i < len; ++i) body.code[start + i] = 0x00;
                    touched += len;
                }
                for (const ca::u32 tail : site->keptTails) {
                    if (tail < body.code.size()) {
                        body.code[tail] = 0x00;
                        ++touched;
                    }
                }
                if (touched != 0) changed = true;
            }
            if (!changed) return;
            auto encoded = jbc::core::writeClass(cf);
            if (encoded.is_err()) return;
            evalView.addResource(std::string(uri),
                                 std::make_unique<mjbase::model::ClassResource>(
                                     std::string(uri), std::move(encoded).unwrap()));
        });
        // 未被改写的类按原字节补齐副本（求值接力需要全容器类）。
        container.visitResources([&](std::string_view uri, const mjbase::model::IResource& res) {
            if (res.getType() != mjbase::model::ResourceType::Class) return;
            const auto* cls = ca::core::tag_cast::dyn_cast<
                const mjbase::model::ClassResource>(&res);
            if (cls == nullptr) return;
            if (evalView.getResource(uri) != nullptr) return;
            evalView.addResource(std::string(uri),
                                 std::make_unique<mjbase::model::ClassResource>(
                                     std::string(uri), cls->get()));
        });
    }

    SimExecutor exec(evalView);
    jsim::str::install(exec.session());
    jsim::str::installStringAlloc(exec.session());   // 解密器体 `new String(char[])`
    jsim::num::install(exec.session());
    scan_tools::installPoolEvalMocks(exec.session());

    const std::vector<jsim::SimValue> noArgs;
    for (ca::u32 round = 0; round < 3; ++round) {
        bool progressed = false;
        for (PoolGroup& g : groups) {
            if (g.writeSuspect || g.evalOk) continue;
            const SimOutcome out = exec.invoke(g.owner, "<clinit>()V", noArgs);
            if (!out.ok()) {
                if (evalTrace) {
                    std::fprintf(stderr, "[grunt.PoolArrayFold] eval-bail %s.%s round %u: %s\n",
                                 g.owner.c_str(), g.field.c_str(), round, out.detail.c_str());
                }
                continue;   /// 未收敛：等下一轮（上游池类可能未跑）。
            }
            const auto it = exec.session().statics.find(poolKey(g.owner, g.field));
            if (it == exec.session().statics.end()) continue;
            const jsim::SimValue& v = it->second;
            if (!v.known || v.type != jsim::ValType::Ref
                || v.ref.kind != jsim::RefPayload::Kind::Obj) {
                continue;
            }
            const auto hit = exec.session().heap.find(v.ref.objId);
            if (hit == exec.session().heap.end()) continue;
            std::unordered_map<ca::i32, PlainUnits> elems;
            const ca::i32 n = static_cast<ca::i32>(hit->second.refs.size());
            for (ca::i32 i = 0; i < n; ++i) {
                const jsim::SimValue& e = hit->second.refs[static_cast<ca::usize>(i)];
                if (!e.known || e.type != jsim::ValType::Ref) continue;
                std::string mutf8;
                if (e.ref.kind == jsim::RefPayload::Kind::Str) {
                    mutf8 = e.ref.str;
                } else if (e.ref.kind == jsim::RefPayload::Kind::Obj) {
                    const auto objIt = exec.session().heap.find(e.ref.objId);
                    if (objIt == exec.session().heap.end()) continue;
                    if (objIt->second.className != "java/lang/String") continue;
                    mutf8 = objIt->second.data;
                } else {
                    continue;
                }
                std::vector<ca::u16> units;
                if (!scan_tools::mutf8ToUnits(mutf8, units)) continue;
                elems.emplace(i, std::move(units));
            }
            table[poolKey(g.owner, g.field)] = std::move(elems);
            g.evalOk    = true;
            progressed  = true;
        }
        if (!progressed) break;
    }
    for (const PoolGroup& g : groups) {
        if (g.writeSuspect || g.evalOk) continue;
        ++result.evalBailed;
    }
}

/// @brief 单类三元组扫描（countOnly = residual 模式，不产 Site/Reject）。
void scanClassTriples(ir::ClassSession& s, const std::string& uri,
                      const PlainTable& table, ScanResult& result, bool countOnly,
                      ca::usize& residual)
{
    const std::string selfName(s.unit().symbols().str(s.unit().thisClass));

    // 类级聚合：候选站点先收集，统一入账（residual 口径按最终折叠面）。
    std::vector<Site>   cands;
    std::vector<Reject> localRejects;

    for (ca::usize mi = 0; mi < s.unit().methods.size(); ++mi) {
        if (!ir::hasCode(s, mi)) continue;
        auto promoted = ir::promoteMethod(s, mi);
        if (promoted.is_err()) continue;
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;

        const std::vector<ir::InsnId>& order = cu->dom.order();
        if (order.size() < 3) continue;
        for (ca::usize p = 0; p + 2 < order.size(); ++p) {
            const ir::Insn* head = cu->dom.peek(order[p]);
            if (head == nullptr || head->op != Op::GETSTATIC) continue;
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*head, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field
                || sem.desc != knowledge::kGruntPoolFieldDesc || sem.owner != selfName) continue;
            const std::string fieldName(sem.name);
            const auto        it = table.find(poolKey(selfName, fieldName));
            if (it == table.end()) continue;   /// 求值面类：三元组形状不计入。
            // 三元组紧邻：NOP 跳过；跨度带标签 = 合并点，fail-closed。
            bool            labeled = false;
            ca::usize       midQ = 0, tailQ = 0;
            const ir::Insn* mid  = scan_tools::nextRealInsn(cu->dom, order, p + 1, &midQ, &labeled);
            const ir::Insn* tail =
                (mid == nullptr) ? nullptr : scan_tools::nextRealInsn(cu->dom, order, midQ + 1, &tailQ, &labeled);
            ca::i32         idx  = 0;
            if (labeled || mid == nullptr || tail == nullptr
                || !const_args::decodeConstInt(s, *mid, idx)
                || tail->op != Op::AALOAD) {
                if (labeled && !countOnly) {
                    Reject rej;
                    rej.uri         = uri;
                    rej.methodIndex = mi;
                    rej.anchorSrcPc = head->srcPc;
                    rej.why         = "三元组含合并点标签（fail-closed）";
                    result.rejects.push_back(std::move(rej));
                }
                continue;
            }
            ++result.tripleHits;

            const auto hit = it->second.find(idx);
            if (hit == it->second.end()) {
                ++result.idxMissing;
                if (!countOnly) {
                    Reject rej;
                    rej.uri         = uri;
                    rej.methodIndex = mi;
                    rej.anchorSrcPc = head->srcPc;
                    rej.why         = "下标 " + std::to_string(idx) + " 不在求值终态（fail-closed）";
                    result.rejects.push_back(std::move(rej));
                }
                continue;
            }
            if (countOnly) {
                // residual 口径按最终折叠面：先入候选，聚合判定后计数。
                std::string record;
                if (!rewrite_tools::u16ToUtf8Record(hit->second, record)) continue;
                Site site;
                site.uri         = uri;
                site.methodIndex = mi;
                site.anchorSrcPc = head->srcPc;
                site.owner       = selfName;
                site.fieldName   = fieldName;
                site.index       = idx;
                site.plainUnits  = hit->second;
                site.plaintext   = std::move(record);
                cands.push_back(std::move(site));
                continue;
            }

            std::string record;
            if (!rewrite_tools::u16ToUtf8Record(hit->second, record)) {
                Reject rej;
                rej.uri         = uri;
                rej.methodIndex = mi;
                rej.anchorSrcPc = head->srcPc;
                rej.why         = "池条目 " + std::to_string(idx)
                                  + " 明文含孤立代理，不可记录（fail-closed）";
                localRejects.push_back(std::move(rej));
                continue;
            }
            Site site;
            site.uri         = uri;
            site.methodIndex = mi;
            site.anchorSrcPc = head->srcPc;
            site.owner       = selfName;
            site.fieldName   = fieldName;
            site.index       = idx;
            site.plainUnits  = hit->second;
            site.plaintext   = std::move(record);
            cands.push_back(std::move(site));
        }
    }

    if (countOnly) {
        residual += cands.size();
        return;
    }
    result.sites.insert(result.sites.end(),
                        std::make_move_iterator(cands.begin()),
                        std::make_move_iterator(cands.end()));
    result.rejects.insert(result.rejects.end(),
                          std::make_move_iterator(localRejects.begin()),
                          std::make_move_iterator(localRejects.end()));
}

}   // namespace

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    std::vector<PoolGroup> groups = collectPools(container, result);
    PlainTable             table;
    // 死区计划：求值副本的净化依据（帧锚保留段尾 athrow 在副本里清掉）。
    const junk_region_strip::ScanResult junk = junk_region_strip::scan(container);
    evaluatePools(container, junk, groups, table, result);

    ca::usize sink = 0;   // countOnly 出参：非 count 模式不写
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        scanClassTriples(*session, uri, table, result, false, sink);
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

namespace {

/// @brief 单站点改写（string_pool_fold 同口径：重定位 → 形状复核 → intern →
///        原地 ldc → 擦除）。@return false = 未动（failed 计数）。
bool rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu, const Site& site)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    const std::string selfName(s.unit().symbols().str(s.unit().thisClass));

    ca::usize anchorPos = static_cast<ca::usize>(-1);
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const ir::Insn* in = cu.dom.peek(order[pos]);
        if (in != nullptr && in->op == Op::GETSTATIC && in->srcPc == site.anchorSrcPc) {
            anchorPos = pos;
            break;
        }
    }
    if (anchorPos == static_cast<ca::usize>(-1)) return false;

    bool            labeled = false;
    ca::usize       midQ = 0, tailQ = 0;
    const ir::Insn* head = cu.dom.peek(order[anchorPos]);
    const ir::Insn* mid  = scan_tools::nextRealInsn(cu.dom, order, anchorPos + 1, &midQ, &labeled);
    const ir::Insn* tail =
        (mid == nullptr) ? nullptr : scan_tools::nextRealInsn(cu.dom, order, midQ + 1, &tailQ, &labeled);
    if (head == nullptr || mid == nullptr || tail == nullptr || labeled) return false;
    ir::ClassSession::MemberSem sem;
    ca::i32                     idx = 0;
    if (!s.memberOf(*head, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field
        || sem.owner != selfName || sem.name != site.fieldName
        || sem.desc != knowledge::kGruntPoolFieldDesc
        || !const_args::decodeConstInt(s, *mid, idx)
        || idx != site.index || tail->op != Op::AALOAD) {
        return false;
    }

    const ca::u32 strIdx = rewrite_tools::internPlainString(s.backing().cp, site.plainUnits);
    if (strIdx == 0) return false;

    auto slotR = cu.dom.get(order[anchorPos]);
    if (slotR.is_err()) return false;
    ir::Insn*     anchor   = slotR.unwrap();
    const ir::Insn original = *anchor;

    ir::Insn fresh = ir::CodeEditor::loadConst(strIdx);
    fresh.srcPc    = original.srcPc;
    *anchor        = fresh;

    std::vector<ir::InsnId> victims;
    victims.reserve(tailQ - anchorPos);
    for (ca::usize q = anchorPos + 1; q <= tailQ; ++q) victims.push_back(order[q]);
    if (!cu.dom.eraseBatch(victims).is_ok()) {
        *anchor = original;
        return false;
    }
    return true;
}

}   // namespace

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // 站点按 uri 聚类（万类大 jar 上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit = 0;
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

        ca::usize hit    = 0;
        ca::usize failed = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) {
                failed += sitesOfMethod.size();
                continue;
            }
            ir::CodeUnit* cu = promoted.unwrap().get();
            ca::usize methodHit  = 0;
            ca::usize methodFail = 0;
            for (const Site* site : sitesOfMethod) {
                if (rewriteSite(s, *cu, *site)) ++methodHit;
                else ++methodFail;
            }
            if (methodHit == 0) {
                failed += methodFail;
                continue;
            }
            // 保真写回：lower 按 pcmap 重定位 SMT/LVT/LNT，帧内容不动。折改
            // 写是栈形等价的语义恒等替换，无需帧重推导——Strict 帧再生的
            // CFG 推导对 junk 谓词走廊方法会把 append 帧的 locals 基算丢
            // （样本 C 实证 c.a() VerifyError），语义恒等替换不走推导。
            auto lowered = ir::lowerMethod(s, mi);
            if (lowered.is_err()) {
                // 帧链失败：该方法整方法放弃（session 内半改写态不外泄）。
                failed += methodHit + methodFail;
                continue;
            }
            hit    += methodHit;
            failed += methodFail;
        }
        dirty.failed += failed;
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.hit;
        dirty.classes += 1;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    // proof 是效力检查（不变量复核），不许被 pass 级时间片截短，挂起期间
    // forEachClass 全量放行（residual 统计同样要看到全量类）。
    pass_clock::Suspend                   suspend;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;

    // residual：重跑求值 + 三元组（countOnly），数已收敛池上仍可折而未折的读点。
    ScanResult           counter;
    std::vector<PoolGroup> groups = collectPools(container, counter);
    PlainTable             table;
    const junk_region_strip::ScanResult junk = junk_region_strip::scan(container);
    evaluatePools(container, junk, groups, table, counter);
    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) return;
        scanClassTriples(*session, uri, table, counter, true, residual);
    });
    result.residual = residual;
    return result;
}

}   // namespace grunt::pool_array_fold
