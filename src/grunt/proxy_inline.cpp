/// @file        proxy_inline.cpp
/// @brief       残留 `proxy_inline` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 数据流与扫描
///
/// `Site{caller 三元组, callerMethodIndex, callSrcPc, proxy 三元组, target
/// 三元组, virtualTarget}` → `DirtySet{uris, attempted, applied,
/// failedByReason, skippedByRoundTrip, proxies}`。`callSrcPc` 是原始字节
/// 布局下的 pc（跨会话稳定）。Scan 两遍：定义侧 `buildDefs` 逐方法
/// promote，`shapePlausible` 粗筛（沾形状才进拒绝面，其余静默跳过不刷屏）
/// 后 `decodeProxyBody` 精查；调用点普查 `collectSites` 全容器逐
/// `INVOKESTATIC` 查定义表，命中即站点。
///
/// 初始化暴露检查：按代理分组后，存在 owner 外调用点的代理，owner
/// `<clinit>` 必须中性（缺失或只剩 NOP/RETURN，与 constant_call 同口径，
/// 收口在 `scan_toolbox::classInitNeutral`，owner 级缓存）；非中性**整组
/// 放弃**（只放一半调用点会把初始化时机劈成两半），记 `OwnerInitExposed`。
/// 零引用代理不产站点、不刷拒绝（死码归 DeadMember）。
///
/// ## 改写
///
/// 按 uri → methodIndex 两级聚合；前置检查 `roundTripSafe`。`rewriteSite`
/// 按 `srcPc+op` 定位调用指令，复核成员引用仍是该代理（防同方法前站改写
/// 串位）后原地换：op 按 `virtualTarget` 换 `INVOKESTATIC`/`INVOKEVIRTUAL`，
/// `setMember` 换成目标方法引用（CP intern 去重只追加），`srcPc` 保，锚
/// 在调用指令上的标签/调试行不动。`setMember` 失败整条指令还原。栈效应
/// 不变（恒等转发弹栈/压栈集合一致），栈帧无需修补；lower 失败粒度 = 类，
/// 整类放弃回写。
///
/// **不动代理本体**：内联后代理变零引用死码，交给下游 DeadMember/DeadCode
/// ，链序上本残留排在其前，同一轮就能清掉。
///
/// ## 已知边界
///
/// - 套娃代理单轮只折一跳：rewrite 只消费 ScanResult，转发目标若也是代理，
///   下一轮整链重复时再折。
/// - 伪 `bridge`/`synthetic` 标志不摘（flags_strip 的业务）；摘干净后
///   DeadMember 的 ACC_BRIDGE 豁免不再误保这些死代理体。
/// - 转发目标可解析性不在本残留验：目标可以是容器外方法（JDK / 依赖库），
///   改写只动调用点的方法引用，解析交给加载期。
#include "grunt/proxy_inline.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::proxy_inline {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// 一个形参槽：类型类 + 起始槽位（long/double 占两槽）。
struct ParamSlot
{
    char    kind = 0;   ///< 'I'/'J'/'F'/'D'/'A'（B/C/S/Z 并入 'I'）
    ca::u32 slot = 0;
};

/// 描述符实参区走槽：只认常规描述符；解析不了返回 false（fail-closed）。
bool walkParams(std::string_view desc, std::vector<ParamSlot>& out)
{
    out.clear();
    if (desc.empty() || desc.front() != '(') return false;
    ca::u32   slot = 0;
    ca::usize i    = 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        ParamSlot   p;
        switch (c) {
        case 'B': case 'C': case 'S': case 'Z': case 'I':
            p = ParamSlot{'I', slot};
            slot += 1;
            i += 1;
            break;
        case 'J':
            p = ParamSlot{'J', slot};
            slot += 2;
            i += 1;
            break;
        case 'F':
            p = ParamSlot{'F', slot};
            slot += 1;
            i += 1;
            break;
        case 'D':
            p = ParamSlot{'D', slot};
            slot += 2;
            i += 1;
            break;
        case 'L': {
            const ca::usize end = desc.find(';', i);
            if (end == std::string_view::npos) return false;
            p = ParamSlot{'A', slot};
            slot += 1;
            i = end + 1;
            break;
        }
        case '[': {
            ca::usize j = i;
            while (j < desc.size() && desc[j] == '[') ++j;
            if (j >= desc.size()) return false;
            if (desc[j] == 'L') {
                const ca::usize end = desc.find(';', j);
                if (end == std::string_view::npos) return false;
                i = end + 1;
            } else {
                i = j + 1;
            }
            p = ParamSlot{'A', slot};
            slot += 1;
            break;
        }
        default: return false;
        }
        out.push_back(p);
    }
    return i < desc.size() && desc[i] == ')';
}

/// 描述符返回类型类：'V'/'I'/'J'/'F'/'D'/'A'（B/C/S/Z 并入 'I'）。
char returnKind(std::string_view desc)
{
    const ca::usize rp = desc.rfind(')');
    if (rp == std::string_view::npos || rp + 1 >= desc.size()) return 0;
    switch (desc[rp + 1]) {
    case 'V': return 'V';
    case 'J': return 'J';
    case 'F': return 'F';
    case 'D': return 'D';
    case 'L': case '[': return 'A';
    case 'B': case 'C': case 'S': case 'Z': case 'I': return 'I';
    default: return 0;
    }
}

/// 形参槽该用的装载指令。
Op loadOpOf(char kind)
{
    switch (kind) {
    case 'J': return Op::LLOAD;
    case 'F': return Op::FLOAD;
    case 'D': return Op::DLOAD;
    case 'A': return Op::ALOAD;
    default:  return Op::ILOAD;
    }
}

/// 装载指令是否匹配类型类 + 槽位。两种编码都认：带操作数的
/// `xload n`，与把槽位编码进操作码的紧凑形态 `xload_0..3`（CodeDom 不做
/// 归一化，汇编器产出哪种就是哪种）。
bool loadMatches(const ir::Insn& in, char kind, ca::u32 slot)
{
    const Op direct = loadOpOf(kind);
    if (in.op == direct) return static_cast<ca::u32>(in.a) == slot;
    Op base;
    switch (kind) {
    case 'J': base = Op::LLOAD_0; break;
    case 'F': base = Op::FLOAD_0; break;
    case 'D': base = Op::DLOAD_0; break;
    case 'A': base = Op::ALOAD_0; break;
    default:  base = Op::ILOAD_0; break;
    }
    const int d = static_cast<int>(in.op) - static_cast<int>(base);
    return d >= 0 && d <= 3 && static_cast<ca::u32>(d) == slot;
}

/// 返回类型类该用的返回指令。
Op returnOpOf(char kind)
{
    switch (kind) {
    case 'V': return Op::RETURN;
    case 'J': return Op::LRETURN;
    case 'F': return Op::FRETURN;
    case 'D': return Op::DRETURN;
    case 'A': return Op::ARETURN;
    default:  return Op::IRETURN;
    }
}

/// owner\nname\ndesc 复合键收口在 scan_toolbox（keyOf），与 constant_call /
/// gate_processor_fold 的方法索引及 field_access_proxy / const_method 的定义
/// 表同源。

/// 代理定义：体形成立时记下转发目标。
struct ProxyDef
{
    std::string targetOwner;
    std::string targetName;
    std::string targetDesc;
    bool        virtualTarget = false;
};

/// 定义表：owner\nname\ndesc -> 转发目标。
using ProxyIndex = std::unordered_map<std::string, ProxyDef>;

/// 方法体是否沾直线代理形状：值得进拒绝面（trace 有用）；其余静默跳过。
bool shapePlausible(const ir::CodeUnit& cu)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    if (order.size() < 2) return false;
    const ir::Insn* tail = cu.dom.peek(order.back());
    if (tail == nullptr) return false;
    switch (tail->op) {
    case Op::IRETURN: case Op::LRETURN: case Op::FRETURN: case Op::DRETURN:
    case Op::ARETURN: case Op::RETURN: break;
    default: return false;
    }
    const ir::Insn* fwd = cu.dom.peek(order[order.size() - 2]);
    if (fwd == nullptr) return false;
    return fwd->op == Op::INVOKESTATIC || fwd->op == Op::INVOKEVIRTUAL;
}

/// 体形精查：成立填 `def`，不成立给原因。前提：shapePlausible 已过。
bool decodeProxyBody(ir::ClassSession& s, ir::CodeUnit& cu, std::string_view desc,
                     ProxyDef& def, RejectReason& why)
{
    const ir::CodeDom&             dom   = cu.dom;
    const std::vector<ir::InsnId>& order = dom.order();

    // 直线检查：任何指令带标签 = 有分支/合并点/异常入口，形状失效。
    for (const ir::InsnId id : order) {
        if (!dom.labelsAt(id).empty()) {
            why = RejectReason::BodyNotProxyShape;
            return false;
        }
    }
    if (!cu.exc.empty()) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }

    std::vector<ParamSlot> params;
    if (!walkParams(desc, params) || order.size() != params.size() + 2) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }

    // 实参逐槽过手：类型匹配 + 槽位精确。
    for (ca::usize i = 0; i < params.size(); ++i) {
        const ir::Insn* in = dom.peek(order[i]);
        if (in == nullptr || !loadMatches(*in, params[i].kind, params[i].slot)) {
            why = RejectReason::BodyNotProxyShape;
            return false;
        }
    }

    // 转发指令：只认 static / virtual。
    const ir::Insn* fwd = dom.peek(order[params.size()]);
    if (fwd == nullptr) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }
    ir::ClassSession::MemberSem enc;
    if (!s.memberOf(*fwd, enc) || enc.kind != ir::ClassSession::MemberSem::Kind::Method) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }
    if (fwd->op == Op::INVOKESTATIC) {
        // static 目标：代理描述符 == 目标描述符（实参集合与返回完全一致）。
        if (enc.desc != desc) {
            why = RejectReason::TargetDescMismatch;
            return false;
        }
        def = ProxyDef{std::string(enc.owner), std::string(enc.name), std::string(enc.desc),
                       false};
    } else {
        // virtual 目标：首实参是接收者。代理描述符首参 == L目标owner; 且
        // 余部（实参区 + 返回）与目标描述符一致。
        const ca::usize rp = desc.find(')');
        if (rp == std::string_view::npos || rp < 2) {
            why = RejectReason::BodyNotProxyShape;
            return false;
        }
        const std::string_view region = desc.substr(1, rp - 1);   // "LX;P1..Pn"
        if (region.empty() || region.front() != 'L') {
            why = RejectReason::TargetDescMismatch;
            return false;
        }
        const ca::usize recvEnd = region.find(';');
        if (recvEnd == std::string_view::npos) {
            why = RejectReason::BodyNotProxyShape;
            return false;
        }
        const std::string receiver = std::string(region.substr(0, recvEnd + 1));  // "LX;"
        const std::string expectedRecv = "L" + std::string(enc.owner) + ";";
        const std::string expected = "(" + std::string(region.substr(recvEnd + 1)) +
                                     std::string(desc.substr(rp));                // "(P..)R"
        if (receiver != expectedRecv || enc.desc != expected) {
            why = RejectReason::TargetDescMismatch;
            return false;
        }
        def = ProxyDef{std::string(enc.owner), std::string(enc.name), std::string(enc.desc),
                       true};
    }

    // 返回指令与代理描述符的返回类型吻合。
    const ir::Insn* ret = dom.peek(order.back());
    if (ret == nullptr || ret->op != returnOpOf(returnKind(desc))) {
        why = RejectReason::ReturnMismatch;
        return false;
    }
    return true;
}

void buildDefs(const mjbase::model::ResourceContainer& container, ProxyIndex& defs,
               ca::usize& found, std::vector<RejectedProxy>& rejects)
{
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession&     s    = *session;
        const ir::ClassUnit&  unit = s.unit();

        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            const ir::MethodUnit& mu = unit.methods[mi];
            if ((mu.access & kAccStatic) == 0) continue;
            const std::string_view name = unit.symbols().str(mu.name);
            const std::string_view desc = unit.symbols().str(mu.desc);
            if (!name.empty() && name.front() == '<') continue;
            if (!ir::hasCode(s, mi)) continue;

            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            if (!shapePlausible(*cu)) continue;   ///< 不沾形状：静默跳过，不刷拒绝面

            const auto reject = [&](RejectReason r) {
                rejects.push_back(RejectedProxy{std::string(s.backing().thisName()),
                                                std::string(name), std::string(desc), r});
            };

            if ((mu.access & kAccSynchronized) != 0) {
                reject(RejectReason::ProxySynchronized);
                continue;
            }

            ProxyDef    def;
            RejectReason why = RejectReason::BodyNotProxyShape;
            if (!decodeProxyBody(s, *cu, desc, def, why)) {
                reject(why);
                continue;
            }
            defs.emplace(scan_tools::keyOf(s.backing().thisName(), name, desc), def);
            ++found;
        }
    });
}

/// 调用点普查 + 初始化暴露检查。站点按代理分组收齐后以代理为单位裁决：
/// 调用点越出 owner 类的代理，owner `<clinit>` 必须中性，否则整组放弃。
void collectSites(const mjbase::model::ResourceContainer& container, const ProxyIndex& defs,
                  ScanResult& result)
{
    struct SiteGroup
    {
        std::vector<Site> sites;
        bool              externalCaller = false;
    };
    std::unordered_map<std::string, SiteGroup> groups;

    forEachClass(container, [&](const ClassEntry& entry) {
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

                const auto it = defs.find(scan_tools::keyOf(enc.owner, enc.name, enc.desc));
                if (it == defs.end()) continue;
                const ProxyDef& def = it->second;

                Site site;
                site.callerUri         = std::string(entry.uri);
                site.callerOwner       = std::string(self);
                site.callerMethod      = std::string(name);
                site.callerDesc        = std::string(desc);
                site.callerMethodIndex = mi;
                site.callSrcPc         = in->srcPc;
                site.proxyOwner        = std::string(enc.owner);
                site.proxyName         = std::string(enc.name);
                site.proxyDesc         = std::string(enc.desc);
                site.targetOwner       = def.targetOwner;
                site.targetName        = def.targetName;
                site.targetDesc        = def.targetDesc;
                site.virtualTarget     = def.virtualTarget;

                SiteGroup& group = groups[scan_tools::keyOf(enc.owner, enc.name, enc.desc)];
                group.externalCaller = group.externalCaller || self != enc.owner;
                group.sites.push_back(std::move(site));
            }
        }
    });

    // 初始化暴露检查（带 owner 级缓存）：非中性的代理整组放弃，只放一半
    // 调用点会把初始化时机劈成两半。
    std::unordered_map<std::string, signed char> neutralCache;   // -1 未判 / 0 非 / 1 中性
    std::unordered_set<std::string>              dropped;
    for (auto& [key, group] : groups) {
        if (!group.externalCaller) continue;
        const ca::usize sp1    = key.find('\n');
        const ca::usize sp2    = key.find('\n', sp1 + 1);
        const std::string owner = key.substr(0, sp1);
        const std::string pname = key.substr(sp1 + 1, sp2 - sp1 - 1);
        const std::string pdesc = key.substr(sp2 + 1);

        auto [slotIt, inserted] = neutralCache.try_emplace(owner, static_cast<signed char>(-1));
        signed char& verdict = slotIt->second;
        if (verdict == 0) {
            dropped.insert(key);
            continue;
        }
        if (verdict < 0) {
            bool neutral = false;
            forEachClass(container, [&](const ClassEntry& entry) {
                if (neutral) return;
                auto session = openSession(*entry.bytes);
                if (!session) return;
                if (session->backing().thisName() != owner) return;
                neutral = scan_tools::classInitNeutral(*session);
            });
            verdict = neutral ? 1 : 0;
            if (!neutral) {
                dropped.insert(key);
                result.rejects.push_back(
                    RejectedProxy{owner, pname, pdesc, RejectReason::OwnerInitExposed});
            }
        }
    }

    for (auto& [key, group] : groups) {
        if (dropped.count(key) != 0) continue;
        for (Site& site : group.sites) result.sites.push_back(std::move(site));
    }
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// 改写失败原因（trace 用）。
enum class RwResult : ca::u8 {
    Ok = 0,
    CallNotFound,
    MemberMismatch,
    SlotFail,
};

/// 在已 promote 的调用者方法里原地改指一个调用点；失败不改动 DOM。
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
    if (enc.owner != site.proxyOwner || enc.name != site.proxyName ||
        enc.desc != site.proxyDesc) {
        return RwResult::MemberMismatch;
    }

    // 原地改写：调用指令的身份与 srcPc 不变，锚在它上面的标签不必迁移；
    // 栈效应不变（弹栈实参集合与压栈返回值完全一致），栈帧无需修补。
    auto slotR = dom.get(order[callPos]);
    if (slotR.is_err()) return RwResult::SlotFail;
    ir::Insn*     insn     = slotR.unwrap();
    const ir::Insn original = *insn;
    insn->op                = site.virtualTarget ? Op::INVOKEVIRTUAL : Op::INVOKESTATIC;
    if (!session.setMember(*insn, ir::ClassSession::MemberSem::Kind::Method,
                           site.targetOwner, site.targetName, site.targetDesc)) {
        *insn = original;   ///< 改不动就还原：失败不留半截指令
        return RwResult::MemberMismatch;
    }
    return RwResult::Ok;
}

}   // namespace

// ============================================================================
// ==== 对外三段 ==============================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    ProxyIndex defs;
    buildDefs(container, defs, result.proxiesFound, result.rejects);
    if (defs.empty()) return result;

    collectSites(container, defs, result);
    return result;
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    std::unordered_set<std::string>                           touchedProxies;
    for (const Site& s : result.sites) {
        byUri[s.callerUri].push_back(&s);
        touchedProxies.insert(scan_tools::keyOf(s.proxyOwner, s.proxyName, s.proxyDesc));
    }

    // 两遍式：先收动作清单，再逐类开会话 → 改写 → lower → 立即回写，
    // 峰值内存是单类会话而不是整 jar 的改写字节（同 const_method 的理由）。
    for (auto& [uri, sites] : byUri) {
        const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
        if (bytes == nullptr) continue;
        auto session = openSession(*bytes);
        if (!session) continue;
        ir::ClassSession& s = *session;

        std::unordered_map<ca::usize, std::vector<const Site*>> byMethod;
        for (const Site* site : sites) byMethod[site->callerMethodIndex].push_back(site);

        // 探针必须看到未改动的字节，所以整类的自检先于任何 promote/改写。
        std::vector<ca::usize> candidates;
        candidates.reserve(byMethod.size());
        for (const auto& slot : byMethod) candidates.push_back(slot.first);
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(*bytes, candidates);

        bool changed   = false;
        bool abandoned = false;   ///< 任一方法 lower 失败：整类放弃回写（粒度=类）
        for (auto& [methodIndex, methodSites] : byMethod) {
            if (safe.find(methodIndex) == safe.end()) {
                dirty.skippedByRoundTrip += methodSites.size();
                continue;
            }
            auto promoted = ir::promoteMethod(s, methodIndex);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            bool methodChanged = false;
            for (const Site* site : methodSites) {
                const RwResult r = rewriteSite(s, *cu, *site);
                ++dirty.attempted;
                if (r == RwResult::Ok) {
                    ++dirty.applied;
                    methodChanged = true;
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
                    std::fprintf(stderr, "[proxy-lower-fail] %s method#%zu err=%s\n",
                                 std::string(s.backing().thisName()).c_str(), methodIndex,
                                 ir::to_string(lowered.unwrap_err()));
                }
                abandoned = true;
                break;
            }
            changed = true;
        }
        if (abandoned || !changed) continue;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) continue;
        replaceClass(container, uri, std::move(encoded).unwrap());
        dirty.uris.push_back(uri);
    }

    dirty.proxies = touchedProxies.size();
    return dirty;
}

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

}   // namespace grunt::proxy_inline
