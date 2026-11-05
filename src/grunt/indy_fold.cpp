/// @file        indy_fold.cpp
/// @brief       invokedynamic 解密折叠：跑 BSM 求目标，就地改指令。
///
/// 三段（SCAN / REWRITE / PROOF）与其余残留同一副骨架：scan 只读、rewrite 只写、
/// proof 只判。差别在于 scan 里多了一步**模拟执行**——目标不是从字节码模式里
/// 推出来的，是把引导方法真跑一遍算出来的。

#include "grunt/indy_fold.hpp"

#include "grunt/iter_budget.hpp"

#include "libca/core/tag_cast.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "grunt/container.hpp"
#include "grunt/jsim_bridge.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "jsim/engine.hpp"
#include "jsim/indy.hpp"
#include "jsim/num.hpp"
#include "jsim/reflect.hpp"
#include "jsim/str.hpp"

namespace grunt::indy_fold {
namespace {

using jbc::core::CpTag;

/// BSM 的三段标准前导参数：Lookup / String(name) / MethodType。
/// JVMS 4.2.2 定的形态，静态参数从第 4 个形参开始一一对应。
constexpr ca::usize kBsmPrologue = 3;

/// MethodHandle 的 reference_kind（JVMS 4.4.8）→ 折叠能用的调用形态。
bool kindOfReferenceKind(ca::u32 referenceKind, jsim::indy::Target::Kind& out)
{
    switch (referenceKind) {
    case 5: out = jsim::indy::Target::Kind::Virtual; return true;    // invokeVirtual
    case 6: out = jsim::indy::Target::Kind::Static; return true;     // invokeStatic
    case 7: out = jsim::indy::Target::Kind::Special; return true;    // invokeSpecial
    case 9: out = jsim::indy::Target::Kind::Interface; return true;  // invokeInterface
    default: return false;   // getField 等：不是方法句柄，折叠不认
    }
}

/// 取类级 BootstrapMethods 的条目表：收口进 scan_toolbox（indy_fold /
/// indy_decode 共用），此处只做引用别名。
const std::vector<jbc::core::BootstrapMethodsAttr::Entry>* bsmEntries(
    jbc::ir::ClassSession& s)
{
    return scan_tools::bsmEntries(s);
}

/// CP 里的静态参数 → 折叠侧的 BsmValue。收不全就标 Unsupported。
BsmValue readBsmArg(const jbc::core::ConstantPool& cp, ca::u32 index)
{
    BsmValue value;
    if (!cp.valid(index)) return value;
    const jbc::core::ConstantPool::Entry& e = cp.at(index);
    switch (e.tag) {
    case CpTag::String:
        value.kind = BsmValue::Kind::Str;
        value.text = std::string(cp.utf8Of(e.a));
        return value;
    case CpTag::Integer:
        value.kind = BsmValue::Kind::Int;
        value.i    = e.ival;
        return value;
    case CpTag::Class:
        value.kind = BsmValue::Kind::Class;
        value.text = std::string(cp.utf8Of(e.a));
        return value;
    case CpTag::MethodType:
        value.kind = BsmValue::Kind::MethodType;
        value.text = std::string(cp.utf8Of(e.a));
        return value;
    case CpTag::MethodHandle:
        value.kind = BsmValue::Kind::MethodHandle;
        return value;
    default: return value;
    }
}

/// 把 BSM 的静态参数灌成 SimValue。前三个形参（Lookup/name/MethodType）给 Unknown
/// ——折叠不需要它们，而「不知道」比「编一个」安全。
bool materializeBsmArgs(const std::vector<jsim::ValType>& params,
                        const std::vector<BsmValue>& args, jsim::Session& session,
                        std::vector<jsim::SimValue>& out, std::string& why)
{
    out.clear();
    out.reserve(params.size());
    for (ca::usize i = 0; i < params.size() && i < kBsmPrologue; ++i) {
        out.push_back(jsim::SimValue::unknownOf(params[i]));
    }
    if (args.size() + kBsmPrologue != params.size()) {
        why = "BSM 静态参数个数与描述符不符";
        return false;
    }
    for (ca::usize i = 0; i < args.size(); ++i) {
        const BsmValue& arg = args[i];
        switch (arg.kind) {
        case BsmValue::Kind::Str:
            out.push_back(jsim::SimValue::knownStr(arg.text));
            break;
        case BsmValue::Kind::Int:
            out.push_back(jsim::SimValue::knownI32(arg.i));
            break;
        case BsmValue::Kind::Class:
            out.push_back(jsim::SimValue::knownObj(session.classObjectOf(arg.text)));
            break;
        case BsmValue::Kind::MethodType:
            out.push_back(jsim::indy::makeMethodType(session, arg.text));
            break;
        default:
            why = "BSM 静态参数形态不支持";
            return false;
        }
    }
    return true;
}

/// 执行器 + 扩展包。分层红线的示范用法：执行器自己不装包，谁需要谁装。
class Engine
{
public:
    explicit Engine(const mjbase::model::ResourceContainer& container) : exec_(container)
    {
        jsim::str::install(exec_.session());
        jsim::num::install(exec_.session());
        // 折叠不需要注解元数据：BSM 只用 forName/getClassLoader 这两个静态面。
        jsim::reflect::install(exec_.session(), [](const std::string&)
                                   -> const jsim::reflect::ClassMeta* { return nullptr; });
        jsim::indy::install(exec_.session());
    }

    SimExecutor&       exec() noexcept { return exec_; }
    jsim::Session&     session() noexcept { return exec_.session(); }
    const SimExecutor::Stats& stats() const noexcept { return exec_.stats(); }

private:
    SimExecutor exec_;
};

/// 跑 BSM，取它返回的 CallSite 目标。
bool runBsm(Engine& engine, const std::string& hostUri, const Site& site,
            jsim::indy::Target& target, std::string& why)
{
    std::vector<jsim::ValType> params;
    if (!jsim_bridge::paramTypesOfDesc(site.bsmDesc, params)) {
        why = "BSM 描述符解析失败";
        return false;
    }
    std::vector<jsim::SimValue> args;
    if (!materializeBsmArgs(params, site.args, engine.session(), args, why)) return false;

    // BSM 可能在别的类里；按它自己的 owner 定位，别假设同类。
    const SimOutcome out =
        engine.exec().invoke(site.bsmOwner, site.bsmName + site.bsmDesc, args);
    if (!out.ok()) {
        why = "BSM 未收敛：" + out.detail;
        return false;
    }
    if (!jsim::indy::isCallSite(engine.session(), out.value)) {
        why = "BSM 返回值不是 CallSite";
        return false;
    }
    if (!jsim::indy::targetOf(engine.session(), out.value, target)) {
        why = "CallSite 句柄载荷不完整";
        return false;
    }
    (void)hostUri;
    return true;
}

/// 一个调用点能不能折。三条判据见头文件。
bool judge(const Site& site, const jsim::indy::Target& target, std::string& why)
{
    if (target.kind != jsim::indy::Target::Kind::Static
        && target.kind != jsim::indy::Target::Kind::Virtual) {
        why = "目标不是静态/虚调用";
        return false;
    }
    if (target.desc != site.indyDesc) {
        why = "句柄描述符与调用点不符（asType 做了适配）：" + target.desc + " vs "
              + site.indyDesc;
        return false;
    }
    return true;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budgetTemplate)
{
    ScanResult result;
    Engine     engine(container);
    ca::usize  named = 0;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        std::shared_ptr<jbc::ir::ClassSession> session = openSession(*entry.bytes);
        if (!session) return;

        const std::vector<jbc::core::BootstrapMethodsAttr::Entry>* bsms =
            bsmEntries(*session);
        if (bsms == nullptr) return;

        const jbc::core::ConstantPool& cp   = session->backing().cp;
        const jbc::ir::ClassUnit&      unit = session->unit();
        const std::string              owner(unit.symbols().str(unit.thisClass));

        bool any = false;
        bool overBudget = false;   ///< 本类站点数超预算：余下站点不再跑 BSM
        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置记账
        for (ca::usize mi = 0; mi < unit.methods.size() && !overBudget; ++mi) {
            if (!jbc::ir::hasCode(*session, mi)) continue;
            auto promoted = jbc::ir::promoteMethod(*session, mi);
            if (promoted.is_err()) continue;
            jbc::ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            const std::vector<jbc::ir::InsnId>& order = cu->dom.order();
            for (ca::usize pc = 0; pc < order.size() && !overBudget; ++pc) {
                const jbc::ir::Insn* in = cu->dom.peek(order[pc]);
                if (in == nullptr || in->op != jbc::core::Op::INVOKEDYNAMIC) continue;
                // 站点记账（1 站点 = 一次 BSM 模拟，引擎内部另有 4096 步上限）。
                if (!budget.tick()) {
                    overBudget = true;
                    ++result.classesSkippedByBudget;
                    iter_budget::reportExhausted("grunt.IndyFold", "scan", uri, budget,
                                                 named);
                    break;
                }

                Site site;
                site.uri         = uri;
                site.owner       = owner;
                site.methodIndex = mi;
                site.methodName  = std::string(unit.symbols().str(unit.methods[mi].name));
                site.methodDesc  = std::string(unit.symbols().str(unit.methods[mi].desc));
                site.insnPc      = pc;
                any              = true;

                if (!cp.valid(in->cp)) {
                    site.why = "indy 的 CP 索引无效";
                    result.sites.push_back(std::move(site));
                    continue;
                }
                const jbc::core::ConstantPool::Entry& indy = cp.at(in->cp);
                if (indy.tag != CpTag::InvokeDynamic) {
                    site.why = "CP 条目不是 InvokeDynamic";
                    result.sites.push_back(std::move(site));
                    continue;
                }
                if (indy.a >= bsms->size()) {
                    site.why = "bootstrap 下标越界";
                    result.sites.push_back(std::move(site));
                    continue;
                }
                // 调用点名/描述符在 NameAndType 里。
                if (cp.valid(indy.b)) {
                    const jbc::core::ConstantPool::Entry& nat = cp.at(indy.b);
                    if (nat.tag == CpTag::NameAndType) {
                        site.indyName = std::string(cp.utf8Of(nat.a));
                        site.indyDesc = std::string(cp.utf8Of(nat.b));
                    }
                }

                const jbc::core::BootstrapMethodsAttr::Entry& bsm = (*bsms)[indy.a];
                // BSM 句柄：MethodHandle → Methodref。
                if (cp.valid(bsm.methodRef)) {
                    const jbc::core::ConstantPool::Entry& mh = cp.at(bsm.methodRef);
                    if (mh.tag == CpTag::MethodHandle) {
                        jsim_bridge::detail::memberRefOf(cp, mh.b, site.bsmOwner, site.bsmName,
                                                         site.bsmDesc);
                    }
                }
                for (ca::u32 argCp : bsm.args) site.args.push_back(readBsmArg(cp, argCp));

                if (site.bsmOwner.empty() || site.bsmName.empty()) {
                    site.why = "BSM 句柄解析失败";
                    result.sites.push_back(std::move(site));
                    continue;
                }

                jsim::indy::Target target;
                std::string       why;
                if (!runBsm(engine, uri, site, target, why)) {
                    site.why = std::move(why);
                    ++result.bsmFailed;
                    result.sites.push_back(std::move(site));
                    continue;
                }
                site.target = target;
                if (!judge(site, target, why)) {
                    site.why = std::move(why);
                    result.sites.push_back(std::move(site));
                    continue;
                }
                site.foldable = true;
                ++result.foldable;
                result.sites.push_back(std::move(site));
            }
            // promoted 在这里释放：scan 不写回，方法体保持原样。
        }
        if (any) ++result.classes;
    });

    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // 按类聚合，再按方法聚合：一个方法只 promote/lower 一次。
    std::unordered_map<std::string, std::vector<const Site*>> byClass;
    for (const Site& site : result.sites) {
        if (!site.foldable) continue;
        byClass[site.uri].push_back(&site);
    }

    for (const auto& group : byClass) {
        const std::string uri = group.first;
        const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
        if (bytes == nullptr) continue;
        std::shared_ptr<jbc::ir::ClassSession> session = openSession(*bytes);
        if (!session) continue;

        std::unordered_map<ca::usize, std::vector<const Site*>> byMethod;
        for (const Site* site : group.second) byMethod[site->methodIndex].push_back(site);

        bool     changed   = false;
        ca::usize folded    = 0;
        bool     abandoned = false;

        for (const auto& method : byMethod) {
            auto promoted = jbc::ir::promoteMethod(*session, method.first);
            if (promoted.is_err()) continue;
            jbc::ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            const std::vector<jbc::ir::InsnId>& order = cu->dom.order();
            bool methodChanged = false;
            for (const Site* site : method.second) {
                if (site->insnPc >= order.size()) continue;
                auto slot = cu->dom.get(order[site->insnPc]);
                if (slot.is_err()) continue;
                jbc::ir::Insn* in = slot.unwrap();
                if (in->op != jbc::core::Op::INVOKEDYNAMIC) continue;   // 已被别的站点改过

                const bool isStatic = site->target.kind == jsim::indy::Target::Kind::Static;
                in->op = isStatic ? jbc::core::Op::INVOKESTATIC : jbc::core::Op::INVOKEVIRTUAL;
                // 操作数整族换成目标方法引用：intern 去重，CP 只追加。
                if (!session->setMember(*in, jbc::ir::ClassSession::MemberSem::Kind::Method,
                                        site->target.owner, site->target.name,
                                        site->target.desc)) {
                    // 改不动就把 op 还原，保证「失败不留半截指令」。
                    in->op = jbc::core::Op::INVOKEDYNAMIC;
                    continue;
                }
                methodChanged = true;
                ++folded;
            }
            if (!methodChanged) continue;

            auto lowered = jbc::ir::lowerMethod(*session, method.first);
            if (lowered.is_err()) {
                if (std::getenv("GRUNT_DEBUG_INDY_FOLD") != nullptr) {
                    std::fprintf(stderr, "[indy-fold-lower-fail] %s method#%zu err=%s\n",
                                 uri.c_str(), method.first,
                                 std::string(jbc::ir::to_string(lowered.unwrap_err())).c_str());
                }
                // 粒度 = 类：任一方法写不回就整类放弃，避免半改的类落盘。
                abandoned = true;
                break;
            }
            changed = true;
        }

        if (abandoned || !changed) continue;
        auto encoded = jbc::core::writeClass(session->backing());
        if (encoded.is_err()) continue;
        replaceClass(container, uri, std::move(encoded).unwrap());
        dirty.uris.push_back(uri);
        dirty.sites += folded;
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
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::indy_fold
