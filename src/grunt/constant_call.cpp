/// @file        constant_call.cpp
/// @brief       残留 `constant_call` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/constant_call.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

#include "jsim/indy.hpp"
#include "jsim/num.hpp"
#include "jsim/reflect.hpp"
#include "jsim/str.hpp"

namespace grunt::constant_call {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

/// 扫描面 fail-closed 包裹：单个类里的任何异常只丢该类的候选，不放大成
/// 整条管线失败。本残留是全局 pass，异常会一路穿到 mjbase 管线的
/// `catch (...)`，CLI 直接 exit 3（「unknown exception」整包拒绝）——
/// 对齐仓内保守口径（rewrite_toolbox::mutateMethodWithFrames 的两级收口）：
/// std::exception 带 what()，其余兜底；该类跳过并打 stderr 日志。
template <class Fn>
bool scanGuard(std::string_view stage, std::string_view uri, Fn&& fn)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[grunt.ConstantCall] %.*s skip class %.*s: %s\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[grunt.ConstantCall] %.*s skip class %.*s: unknown exception\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data());
    }
    return false;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// 返回类别（'V' 表示 void）。
char returnChar(std::string_view desc)
{
    const ca::usize rp = desc.find(')');
    if (rp == std::string_view::npos || rp + 1 >= desc.size()) return 'V';
    return desc[rp + 1];
}

struct MethodLoc
{
    std::string owner;
    std::string desc;
    ca::usize   index  = 0;
    ca::u32     access = 0;
};

/// 容器方法索引：owner\nname\ndesc -> 声明位置。
using MethodIndex = std::unordered_map<std::string, MethodLoc>;

std::string keyOf(std::string_view owner, std::string_view name, std::string_view desc)
{
    std::string k;
    k.reserve(owner.size() + name.size() + desc.size() + 2);
    k.append(owner);
    k.push_back('\n');
    k.append(name);
    k.push_back('\n');
    k.append(desc);
    return k;
}

void buildIndex(const mjbase::model::ResourceContainer& container, MethodIndex& index)
{
    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("index", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const ir::ClassUnit&   unit = session->unit();
            const std::string_view self = session->backing().thisName();
            for (ca::usize i = 0; i < unit.methods.size(); ++i) {
                const ir::MethodUnit&  mu   = unit.methods[i];
                const std::string_view name = unit.symbols().str(mu.name);
                const std::string_view desc = unit.symbols().str(mu.desc);
                index.emplace(keyOf(self, name, desc),
                              MethodLoc{std::string(self), std::string(desc), i, mu.access});
            }
        });
    });
}

/// 被调类的 `<clinit>` 是否「中性」（缺失，或只剩 NOP/RETURN 且无异常表）。
///
/// 这是保守面而不是判据：删掉 invokestatic 会改变类初始化时机，只有确认
/// 初始化器什么都不做时才允许折叠。
bool classInitNeutral(ir::ClassSession& session)
{
    const ir::ClassUnit& unit = session.unit();
    ca::usize            clinitIndex = kNoIndex;
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) == "<clinit>") {
            clinitIndex = i;
            break;
        }
    }
    if (clinitIndex == kNoIndex) return true;
    if (!ir::hasCode(session, clinitIndex)) return true;

    auto promoted = ir::promoteMethod(session, clinitIndex);
    if (promoted.is_err()) return false;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;
    if (!cu->exc.empty()) return false;

    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (in->op == Op::NOP || in->op == Op::RETURN) continue;
        return false;
    }
    return true;
}

/// 求值段：装好扩展包的 jsim 执行器逐站点跑目标方法；通过者写回 `value`，
/// 失败者转成拒绝记录。类初始化门仍在目标会话上先判。
void evaluateSites(const mjbase::model::ResourceContainer& container,
                   std::vector<Site>& sites, std::vector<RejectedSite>& rejects)
{
    std::unordered_map<std::string, std::vector<ca::usize>> byOwner;
    for (ca::usize i = 0; i < sites.size(); ++i) byOwner[sites[i].targetOwner].push_back(i);

    std::vector<ca::u8> ok(sites.size(), 0);   // 0 待判 / 1 通过 / 2 拒绝

    SimExecutor exec(container);
    jsim::str::install(exec.session());
    jsim::num::install(exec.session());
    // 折叠不需要注解/类元数据：目标按判据是纯算术，反射元数据无人读。
    jsim::reflect::install(exec.session(), [](const std::string&)
                               -> const jsim::reflect::ClassMeta* { return nullptr; });
    jsim::indy::install(exec.session());

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("evaluate", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const std::string_view self = session->backing().thisName();

            const auto group = byOwner.find(std::string(self));
            if (group == byOwner.end()) return;

            if (!classInitNeutral(*session)) {
                for (const ca::usize si : group->second) {
                    rejects.push_back(RejectedSite{sites[si].callerOwner, sites[si].callerMethod,
                                                   sites[si].callSrcPc,
                                                   RejectReason::ClassInitPresent});
                    ok[si] = 2;
                }
                return;
            }

            for (const ca::usize si : group->second) {
                Site& site = sites[si];

                if (!ir::hasCode(*session, site.targetMethodIndex)) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetNoCode});
                    ok[si] = 2;
                    continue;
                }

                const SimOutcome out =
                    exec.invoke(site.targetOwner, site.targetName + site.targetDesc, site.args);
                if (out.kind == SimOutcome::Kind::Void) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetVoid});
                    ok[si] = 2;
                    continue;
                }
                if (!out.ok()) {
                    rejects.push_back(RejectedSite{
                        site.callerOwner, site.callerMethod, site.callSrcPc,
                        out.kind == SimOutcome::Kind::LimitHit ? RejectReason::EvalStepLimit
                                                               : RejectReason::EvalBadState});
                    ok[si] = 2;
                    continue;
                }
                if (!out.value.known || out.value.type == jsim::ValType::Ref) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc,
                                                   RejectReason::ValueKindUnsupported});
                    ok[si] = 2;
                    continue;
                }
                site.value = out.value;
                ok[si]     = 1;
            }
        });
    });

    // 没被任何目标类覆盖的站点（目标类解析失败等）统一拒绝。
    std::vector<Site> kept;
    kept.reserve(sites.size());
    for (ca::usize i = 0; i < sites.size(); ++i) {
        if (ok[i] == 1) {
            kept.push_back(std::move(sites[i]));
        } else if (ok[i] == 0) {
            rejects.push_back(RejectedSite{sites[i].callerOwner, sites[i].callerMethod,
                                           sites[i].callSrcPc, RejectReason::TargetNoCode});
        }
    }
    sites = std::move(kept);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// 改写失败原因（trace 用）。
enum class RwResult : ca::u8 {
    Ok = 0,
    CallNotFound,
    MemberMismatch,
    ArgsMismatch,
    LoadFail,
    InsertFail,
    LabelBlocked,
    EraseFail,
};

// ---- 诊断：谁在指「将被擦除的 pc」（GRUNT_DEBUG_PCREFS）---------------------
//
// 擦掉指令后这些旧 pc 不在 lower 的 pcmap 里，fail-closed 重定位会直接拒
// （RelocationBadTarget）。这个转储把「哪类属性、多少行」摆出来，用来判
// 该由改写侧一并作废，还是该整站点放弃。

bool pcIn(const std::vector<ca::u32>& dead, ca::u32 pc)
{
    return std::find(dead.begin(), dead.end(), pc) != dead.end();
}

void dumpPcRefs(ir::ClassSession& session, ca::usize methodIndex,
                const std::vector<ca::u32>& dead)
{
    if (std::getenv("GRUNT_DEBUG_PCREFS") == nullptr || dead.empty()) return;

    jbc::core::ClassFile& cf = session.backing();
    const ir::ClassUnit&  u  = session.unit();
    if (methodIndex >= u.methods.size()) return;
    const ca::usize backing = u.methods[methodIndex].backingIndex;
    if (backing >= cf.methods.size()) return;

    auto&          attrs = cf.methods[backing].attrs;
    ca::usize      codeSlot = kNoIndex;
    for (ca::usize i = 0; i < attrs.size(); ++i) {
        if (cf.utf8(attrs[i].nameCp) == "Code") {
            codeSlot = i;
            break;
        }
    }
    if (codeSlot == kNoIndex) return;
    auto d = cf.decodeAttr(attrs[codeSlot]);
    if (d.is_err()) return;
    auto& code = static_cast<jbc::core::CodeAttr&>(*d.unwrap());

    std::fprintf(stderr, "[pcrefs] dead=");
    for (const ca::u32 pc : dead) std::fprintf(stderr, " %u", pc);
    std::fprintf(stderr, " | codeLen=%zu nested=%zu\n", code.code.size(), code.attrs.size());

    for (jbc::core::AttrSlot& nested : code.attrs) {
        auto nd = cf.decodeAttr(nested);
        if (nd.is_err()) continue;
        jbc::core::AttrBody* nb = nd.unwrap();
        switch (nb->kind()) {
        case jbc::core::AttrKind::LineNumberTable: {
            auto& lnt = static_cast<jbc::core::LineNumberTableAttr&>(*nb);
            ca::usize hit = 0;
            for (const auto& row : lnt.rows) hit += pcIn(dead, row.startPc) ? 1 : 0;
            std::fprintf(stderr, "[pcrefs]   LNT rows=%zu dead=%zu\n", lnt.rows.size(), hit);
            break;
        }
        case jbc::core::AttrKind::LocalVariableTable: {
            auto& lvt = static_cast<jbc::core::LocalVariableTableAttr&>(*nb);
            ca::usize hitStart = 0, hitEnd = 0;
            for (const auto& row : lvt.rows) {
                hitStart += pcIn(dead, row.startPc) ? 1 : 0;
                hitEnd += pcIn(dead, row.startPc + row.length) ? 1 : 0;
            }
            std::fprintf(stderr, "[pcrefs]   LVT rows=%zu deadStart=%zu deadEnd=%zu\n",
                         lvt.rows.size(), hitStart, hitEnd);
            break;
        }
        case jbc::core::AttrKind::StackMapTable: {
            auto&     smt  = static_cast<jbc::core::StackMapTableAttr&>(*nb);
            ca::usize hit  = 0;
            ca::u32   prev = 0;   // 首帧的前一帧 pc 约定为 -1，故 prev = -1 的等价起点
            for (const auto& fr : smt.frames) {
                const ca::u32 abs = jbc::core::smtAbsPc(prev, fr.offset);
                hit += pcIn(dead, abs) ? 1 : 0;
                prev = abs;
            }
            std::fprintf(stderr, "[pcrefs]   SMT frames=%zu dead=%zu\n", smt.frames.size(), hit);
            break;
        }
        default:
            std::fprintf(stderr, "[pcrefs]   kind=%d\n", static_cast<int>(nb->kind()));
            break;
        }
    }
}

/// 在已 promote 的调用者方法里改写一个站点；失败不改动 DOM。
RwResult rewriteSite(ir::ClassSession& session, ir::CodeUnit& cu, const Site& site)
{
    ir::CodeDom&                   dom   = cu.dom;
    const std::vector<ir::InsnId>& order = dom.order();

    ca::usize callPos = kNoIndex;
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = dom.peek(order[i]);
        if (in != nullptr && in->op == Op::INVOKESTATIC && in->srcPc == site.callSrcPc) {
            callPos = i;
            break;
        }
    }
    if (callPos == kNoIndex) return RwResult::CallNotFound;

    const ir::Insn* call = dom.peek(order[callPos]);
    ir::ClassSession::MemberSem enc;
    if (!session.memberOf(*call, enc)) return RwResult::MemberMismatch;
    if (enc.owner != site.targetOwner || enc.name != site.targetName ||
        enc.desc != site.targetDesc) {
        return RwResult::MemberMismatch;
    }

    std::vector<ir::InsnId>     producers;
    std::vector<jsim::SimValue> args;
    if (!const_args::collectArgs(session, dom, order, callPos, const_args::argSlots(enc.desc),
                                 producers, args)) {
        return RwResult::ArgsMismatch;
    }
    if (args.size() != site.args.size()) return RwResult::ArgsMismatch;
    for (ca::usize i = 0; i < args.size(); ++i) {
        if (!const_args::sameValue(args[i], site.args[i])) return RwResult::ArgsMismatch;
    }

    ir::Insn load;
    if (!rewrite_tools::makeConstLoad(session, site.value, load)) return RwResult::LoadFail;

    // order 是 DOM 的活视图：任何结构变更之后下标都会位移，先把句柄取出来。
    const ir::InsnId callId  = order[callPos];
    const ir::InsnId firstId = producers.front();

    // 区间里除首条以外的指令上不许有标签：有标签说明分支/switch/异常范围从
    // 区间中段进入，擦掉会让目标悬垂。首条不受此限——它原地改写，身份不变。
    for (ca::usize i = 1; i < producers.size(); ++i) {
        if (!dom.labelsAt(producers[i]).empty()) return RwResult::LabelBlocked;
    }
    if (!dom.labelsAt(callId).empty()) return RwResult::LabelBlocked;

    // 首条**原地改写**成常量装载，而不是「前面插一条 + 删掉首条」：
    // 插入路径把 srcPc 强制清成 kNoPc（合成指令没有旧位置，CodeDom 的硬
    // 不变量），而 LNT/LVT/StackMapTable 的重定位全靠旧 pc 命中 pcmap——
    // 删掉首条会让锚在区间起点的行全部失去落点，lower 只能整体失败。
    // 原地改写保住它的 srcPc，锚在它上面的标签也因此不必迁移。
    auto slotR = dom.get(firstId);
    if (slotR.is_err()) return RwResult::InsertFail;
    ir::Insn*      first   = slotR.unwrap();
    const ir::Insn original = *first;
    const ca::u32  keepPc   = first->srcPc;
    *first                  = load;
    first->srcPc            = keepPc;

    std::vector<ir::InsnId> victims(producers.begin() + 1, producers.end());
    victims.push_back(callId);

    if (std::getenv("GRUNT_DEBUG_PCREFS") != nullptr) {
        std::vector<ca::u32> dead;
        for (const ir::InsnId id : victims) {
            const ir::Insn* v = dom.peek(id);
            if (v != nullptr && v->srcPc != jbc::ir::kNoPc) dead.push_back(v->srcPc);
        }
        dumpPcRefs(session, site.callerMethodIndex, dead);
    }

    if (dom.eraseBatch(victims).is_err()) {
        *first = original;   // 回滚：DOM 保持原样
        return RwResult::EraseFail;
    }
    return RwResult::Ok;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

}   // namespace

// ============================================================================
// ==== 对外三段 ==============================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    MethodIndex index;
    buildIndex(container, index);

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("scan", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            ir::ClassSession&     s    = *session;
            const ir::ClassUnit&  unit = s.unit();
            const std::string_view self = s.backing().thisName();

            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                if (!ir::hasCode(s, mi)) continue;
                auto promoted = ir::promoteMethod(s, mi);
                if (promoted.is_err()) continue;
                ir::CodeUnit* cu = promoted.unwrap().get();
                if (cu == nullptr) continue;

                const std::string_view         name  = unit.symbols().str(unit.methods[mi].name);
                const std::string_view         desc  = unit.symbols().str(unit.methods[mi].desc);
                const std::vector<ir::InsnId>& order = cu->dom.order();

                for (ca::usize pos = 0; pos < order.size(); ++pos) {
                    const ir::Insn* in = cu->dom.peek(order[pos]);
                    if (in == nullptr || in->op != Op::INVOKESTATIC) continue;

                    ir::ClassSession::MemberSem enc;
                    if (!s.memberOf(*in, enc)) continue;
                    if (enc.kind != ir::ClassSession::MemberSem::Kind::Method) continue;

                    const auto it = index.find(keyOf(enc.owner, enc.name, enc.desc));
                    if (it == index.end()) continue;   // 容器外目标：不构成候选
                    const MethodLoc& loc = it->second;

                    const auto reject = [&](RejectReason r) {
                        result.rejects.push_back(RejectedSite{
                            std::string(self), std::string(name), in->srcPc, r});
                    };

                    if ((loc.access & kAccStatic) == 0) {
                        reject(RejectReason::TargetNotStatic);
                        continue;
                    }
                    if ((loc.access & kAccSynchronized) != 0) {
                        reject(RejectReason::TargetSynchronized);
                        continue;
                    }
                    if (!enc.name.empty() && enc.name.front() == '<') {
                        reject(RejectReason::TargetInitializer);
                        continue;
                    }
                    if (returnChar(enc.desc) == 'V') {
                        reject(RejectReason::TargetVoid);
                        continue;
                    }

                    std::vector<ir::InsnId>     producers;
                    std::vector<jsim::SimValue> args;
                    if (!const_args::collectArgs(s, cu->dom, order, pos,
                                                 const_args::argSlots(enc.desc),
                                                 producers, args)) {
                        reject(RejectReason::ArgsNotConstant);
                        continue;
                    }

                    Site site;
                    site.callerUri         = std::string(entry.uri);
                    site.callerOwner       = std::string(self);
                    site.callerMethod      = std::string(name);
                    site.callerDesc        = std::string(desc);
                    site.callerMethodIndex = mi;
                    site.callSrcPc         = in->srcPc;
                    site.targetOwner       = std::string(enc.owner);
                    site.targetName        = std::string(enc.name);
                    site.targetDesc        = std::string(enc.desc);
                    site.targetMethodIndex = loc.index;
                    site.args              = std::move(args);
                    result.sites.push_back(std::move(site));
                }
            }
        });
    });

    evaluateSites(container, result.sites, result.rejects);
    return result;
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    // 按调用类分组：一次会话内改完该类的全部站点，再 lower + 回写。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& s : result.sites) byUri[s.callerUri].push_back(&s);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("rewrite", entry.uri, [&] {
        const auto it = byUri.find(std::string(entry.uri));
        if (it == byUri.end()) return;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::unordered_map<ca::usize, std::vector<const Site*>> byMethod;
        for (const Site* site : it->second) byMethod[site->callerMethodIndex].push_back(site);

        // 探针必须看到未改动的字节，所以整类的自检先于任何 promote/改写。
        std::vector<ca::usize> candidates;
        candidates.reserve(byMethod.size());
        for (const auto& slot : byMethod) candidates.push_back(slot.first);
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(*entry.bytes, candidates);

        bool changed = false;
        for (auto& [methodIndex, sites] : byMethod) {
            if (safe.find(methodIndex) == safe.end()) {
                dirty.skippedByRoundTrip += sites.size();
                continue;
            }
            auto promoted = ir::promoteMethod(s, methodIndex);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            bool methodChanged = false;
            for (const Site* site : sites) {
                const RwResult r = rewriteSite(s, *cu, *site);
                ++dirty.attempted;
                if (r == RwResult::Ok) {
                    ++dirty.applied;
                    methodChanged = true;
                    if (std::getenv("GRUNT_DEBUG_FOLD") != nullptr) {
                        // 逐条落「折成了什么」：用来和 JVM 反射求值对拍。
                        std::fprintf(stderr, "[fold] %s.%s pc=%u -> %s  = ", site->callerOwner.c_str(),
                                     site->callerMethod.c_str(), site->callSrcPc,
                                     site->targetName.c_str());
                        for (const jsim::SimValue& arg : site->args) {
                            switch (arg.type) {
                            case jsim::ValType::I32:
                                std::fprintf(stderr, "%d ", static_cast<ca::i32>(arg.intBits));
                                break;
                            case jsim::ValType::I64:
                                std::fprintf(stderr, "%lld ", static_cast<long long>(arg.intBits));
                                break;
                            case jsim::ValType::Ref:
                                if (arg.ref.kind == jsim::RefPayload::Kind::Null) {
                                    std::fprintf(stderr, "null ");
                                } else {
                                    std::fprintf(stderr, "? ");
                                }
                                break;
                            default: std::fprintf(stderr, "? "); break;
                            }
                        }
                        switch (site->value.type) {
                        case jsim::ValType::I32:
                            std::fprintf(stderr, "=> %d\n",
                                         static_cast<ca::i32>(site->value.intBits));
                            break;
                        case jsim::ValType::I64:
                            std::fprintf(stderr, "=> %lld\n",
                                         static_cast<long long>(site->value.intBits));
                            break;
                        default: std::fprintf(stderr, "=> ?\n"); break;
                        }
                    }
                } else {
                    const ca::usize idx = static_cast<ca::usize>(r);
                    if (dirty.failedByReason.size() <= idx) dirty.failedByReason.resize(idx + 1, 0);
                    ++dirty.failedByReason[idx];
                }
            }
            if (!methodChanged) continue;
            auto lowered = ir::lowerMethod(s, methodIndex);
            if (lowered.is_err()) {
                if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                    std::fprintf(stderr, "[lower-fail] %s method#%zu err=%s\n",
                                 std::string(s.backing().thisName()).c_str(), methodIndex,
                                 ir::to_string(lowered.unwrap_err()));
                }
                return;   // 整类放弃
            }
            changed = true;
        }
        if (!changed) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                std::fprintf(stderr, "[write-fail] %s err=%s\n",
                             std::string(s.backing().thisName()).c_str(),
                             jbc::core::to_string(encoded.unwrap_err()));
            }
            return;
        }
        pending.push_back(PendingWrite{std::string(entry.uri), std::move(encoded).unwrap()});
        });
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
    }
    return dirty;
}

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    ProofResult result;
    if (countClasses(container) != classesBefore) {
        result.ok = false;
        result.failed.emplace_back("class-count");
    }
    for (const std::string& uri : dirty.uris) {
        bool found = false;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (found || entry.uri != uri) return;
            found = openSession(*entry.bytes) != nullptr;
        });
        if (!found) {
            result.ok = false;
            result.failed.push_back(uri);
        }
    }
    return result;
}

}   // namespace grunt::constant_call
