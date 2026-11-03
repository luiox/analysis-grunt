/// @file        field_access_proxy.cpp
/// @brief       残留 `field_access_proxy` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/field_access_proxy.hpp"

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

namespace grunt::field_access_proxy {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

// ============================================================================
// ==== SCAN 工具箱（文件内私有） ==============================================
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

/// 装载指令是否匹配「类型类 + 槽位」。两种编码都认：带操作数的
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

/// 字段访问种类该落的指令。
Op fieldOpOf(FieldKind kind)
{
    switch (kind) {
    case FieldKind::GetStatic: return Op::GETSTATIC;
    case FieldKind::PutStatic: return Op::PUTSTATIC;
    case FieldKind::GetField:  return Op::GETFIELD;
    case FieldKind::PutField:  return Op::PUTFIELD;
    }
    return Op::GETSTATIC;
}

/// 字段访问指令对应的种类；非字段访问返回 false。
bool kindOfFieldOp(Op op, FieldKind& out)
{
    switch (op) {
    case Op::GETSTATIC: out = FieldKind::GetStatic; return true;
    case Op::PUTSTATIC: out = FieldKind::PutStatic; return true;
    case Op::GETFIELD:  out = FieldKind::GetField; return true;
    case Op::PUTFIELD:  out = FieldKind::PutField; return true;
    default: return false;
    }
}

/// owner\nname\ndesc 复合键。
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

/// 代理定义：体形成立时记下转发到的真实字段。
struct ProxyDef
{
    std::string fieldOwner;
    std::string fieldName;
    std::string fieldDesc;
    FieldKind   kind = FieldKind::GetStatic;
};

/// 定义表：owner\nname\ndesc -> 转发字段。
using ProxyIndex = std::unordered_map<std::string, ProxyDef>;

/// 该种类的代理描述符该有的形状（生成面按字段互证的构造）。
std::string expectedDesc(FieldKind kind, std::string_view fieldOwner,
                         std::string_view fieldDesc)
{
    switch (kind) {
    case FieldKind::GetStatic: return "()" + std::string(fieldDesc);
    case FieldKind::PutStatic: return "(" + std::string(fieldDesc) + ")V";
    case FieldKind::GetField:
        return "(L" + std::string(fieldOwner) + ";)" + std::string(fieldDesc);
    case FieldKind::PutField:
        return "(L" + std::string(fieldOwner) + ";" + std::string(fieldDesc) + ")V";
    }
    return std::string();
}

/// 方法体是否沾「字段代理」形状：值得进拒绝面（trace 有用）；其余静默跳过。
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
    const ir::Insn* mid = cu.dom.peek(order[order.size() - 2]);
    if (mid == nullptr) return false;
    FieldKind kind = FieldKind::GetStatic;
    return kindOfFieldOp(mid->op, kind);
}

/// 体形精查：成立填 `def`，不成立给原因。前提：shapePlausible 已过。
bool decodeProxyBody(ir::ClassSession& s, ir::CodeUnit& cu, std::string_view desc,
                     ProxyDef& def, RejectReason& why)
{
    const ir::CodeDom&             dom   = cu.dom;
    const std::vector<ir::InsnId>& order = dom.order();

    // 直线体：任何指令带标签 = 有分支/合并点/异常入口，形状失效。
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

    // 中位指令 = 唯一的字段访问。
    const ir::Insn* mid = dom.peek(order[params.size()]);
    if (mid == nullptr) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }
    FieldKind kind = FieldKind::GetStatic;
    if (!kindOfFieldOp(mid->op, kind)) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }
    ir::ClassSession::MemberSem enc;
    if (!s.memberOf(*mid, enc) || enc.kind != ir::ClassSession::MemberSem::Kind::Field) {
        why = RejectReason::BodyNotProxyShape;
        return false;
    }

    // 字段与描述符互证：描述符必须恰为生成面按该字段该有的形状。
    if (desc != expectedDesc(kind, enc.owner, enc.desc)) {
        why = RejectReason::FieldDescMismatch;
        return false;
    }

    // 返回指令与代理描述符的返回类型吻合。
    const ir::Insn* ret = dom.peek(order.back());
    if (ret == nullptr || ret->op != returnOpOf(returnKind(desc))) {
        why = RejectReason::ReturnMismatch;
        return false;
    }
    def = ProxyDef{std::string(enc.owner), std::string(enc.name), std::string(enc.desc), kind};
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
            defs.emplace(keyOf(s.backing().thisName(), name, desc), def);
            ++found;
        }
    });
}

/// 调用点普查 + 初始化暴露门。站点按代理分组收齐后以代理为单位裁决：
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

                const auto it = defs.find(keyOf(enc.owner, enc.name, enc.desc));
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
                site.fieldOwner        = def.fieldOwner;
                site.fieldName         = def.fieldName;
                site.fieldDesc         = def.fieldDesc;
                site.kind              = def.kind;

                SiteGroup& group = groups[keyOf(enc.owner, enc.name, enc.desc)];
                group.externalCaller = group.externalCaller || self != enc.owner;
                group.sites.push_back(std::move(site));
            }
        }
    });

    // 初始化暴露门（带 owner 级缓存）：非中性的代理整组放弃——只放一半
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

/// 在已 promote 的调用者方法里原地改回一个调用点；失败不改动 DOM。
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
    // 栈效应不变（getter 弹参压值、setter 弹参不压值，与代理完全一致），
    // 栈帧无需修补。
    auto slotR = dom.get(order[callPos]);
    if (slotR.is_err()) return RwResult::SlotFail;
    ir::Insn*     insn     = slotR.unwrap();
    const ir::Insn original = *insn;
    insn->op                = fieldOpOf(site.kind);
    if (!session.setMember(*insn, ir::ClassSession::MemberSem::Kind::Field,
                           site.fieldOwner, site.fieldName, site.fieldDesc)) {
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
        touchedProxies.insert(keyOf(s.proxyOwner, s.proxyName, s.proxyDesc));
    }

    // 两遍式：先收动作清单，再逐类「开会话 → 改写 → lower → 立即回写」，
    // 峰值内存是单类会话而不是整 jar 的改写字节（同 proxy_inline 的理由）。
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
                    std::fprintf(stderr, "[field-proxy-lower-fail] %s method#%zu err=%s\n",
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

}   // namespace grunt::field_access_proxy
