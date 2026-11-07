/// @file        const_method.cpp
/// @brief       残留 `const_method` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/const_method.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/iter_budget.hpp"
#include "grunt/knowledge/runtime_names.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::const_method {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// @brief 所有者类初始化纯度门：`<clinit>` 缺失，或整体落在纯常量白名单。
///
/// 白名单：常量装载（仅数值类池常量）、数值运算/转换/比较、`putstatic`
/// （目标必须是本类字段——跨类 putstatic 会触发别人的初始化链）、`return`。
/// 直线门：任何指令上带标签即有分支/循环，直接判不纯（循环不终止是副作用）。
bool clinitPure(ir::ClassSession& session)
{
    const ir::ClassUnit& unit          = session.unit();
    const std::string_view self        = session.backing().thisName();
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

    for (const ir::InsnId id : cu->dom.order()) {
        if (!cu->dom.labelsAt(id).empty()) return false;
        const ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) return false;
        switch (in->op) {
        case Op::NOP:
        case Op::ACONST_NULL:
        case Op::ICONST_M1:
        case Op::ICONST_0:
        case Op::ICONST_1:
        case Op::ICONST_2:
        case Op::ICONST_3:
        case Op::ICONST_4:
        case Op::ICONST_5:
        case Op::LCONST_0:
        case Op::LCONST_1:
        case Op::FCONST_0:
        case Op::FCONST_1:
        case Op::FCONST_2:
        case Op::DCONST_0:
        case Op::DCONST_1:
        case Op::BIPUSH:
        case Op::SIPUSH:
        case Op::I2L:
        case Op::I2F:
        case Op::I2D:
        case Op::L2I:
        case Op::L2F:
        case Op::L2D:
        case Op::F2I:
        case Op::F2L:
        case Op::F2D:
        case Op::D2I:
        case Op::D2L:
        case Op::D2F:
        case Op::I2B:
        case Op::I2C:
        case Op::I2S:
        case Op::INEG:
        case Op::LNEG:
        case Op::FNEG:
        case Op::DNEG:
        case Op::IADD:
        case Op::LADD:
        case Op::FADD:
        case Op::DADD:
        case Op::ISUB:
        case Op::LSUB:
        case Op::FSUB:
        case Op::DSUB:
        case Op::IMUL:
        case Op::LMUL:
        case Op::FMUL:
        case Op::DMUL:
        case Op::IDIV:
        case Op::LDIV:
        case Op::FDIV:
        case Op::DDIV:
        case Op::IREM:
        case Op::LREM:
        case Op::FREM:
        case Op::DREM:
        case Op::ISHL:
        case Op::LSHL:
        case Op::ISHR:
        case Op::LSHR:
        case Op::IUSHR:
        case Op::LUSHR:
        case Op::IAND:
        case Op::LAND:
        case Op::IOR:
        case Op::LOR:
        case Op::IXOR:
        case Op::LXOR:
        case Op::LCMP:
        case Op::FCMPL:
        case Op::FCMPG:
        case Op::DCMPL:
        case Op::DCMPG:
        case Op::RETURN: break;
        case Op::LDC:
        case Op::LDC_W:
        case Op::LDC2_W: {
            ir::ClassSession::ConstSem sem;
            if (!session.constOf(*in, sem)) return false;
            switch (sem.kind) {
            case ir::ClassSession::ConstSem::Kind::Int:
            case ir::ClassSession::ConstSem::Kind::Long:
            case ir::ClassSession::ConstSem::Kind::Float:
            case ir::ClassSession::ConstSem::Kind::Double: break;
            default: return false;   ///< String/Class/Handle 等引用常量不猜内容
            }
            break;
        }
        case Op::PUTSTATIC: {
            ir::ClassSession::MemberSem enc;
            if (!session.memberOf(*in, enc)) return false;
            if (enc.owner != self) return false;   ///< 跨类写会触发别人的初始化链
            break;
        }
        default: return false;
        }
    }
    return true;
}

/// @brief 单常量生产解码。`NotConstant`：生产者不是常量形态（静默跳过）；
///        `UnsupportedGetstatic`：getstatic 但目标不可静态编码（进拒绝面）。
enum class ProducerOutcome : ca::u8 { NotConstant, UnsupportedGetstatic, Ok };

ProducerOutcome decodeProducer(ir::ClassSession& session, const ir::Insn& in, ConstValue& out,
                               bool& producedRef)
{
    producedRef = false;
    switch (in.op) {
    case Op::ICONST_M1:
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
        out      = ConstValue{ConstValue::Kind::I32,
                             static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::ICONST_0),
                             0};
        return ProducerOutcome::Ok;
    case Op::BIPUSH:
    case Op::SIPUSH:
        out = ConstValue{ConstValue::Kind::I32, in.a, 0};
        return ProducerOutcome::Ok;
    case Op::LDC:
    case Op::LDC_W: {
        ir::ClassSession::ConstSem sem;
        if (!session.constOf(in, sem)) return ProducerOutcome::NotConstant;
        if (sem.kind != ir::ClassSession::ConstSem::Kind::Int) return ProducerOutcome::NotConstant;
        out = ConstValue{ConstValue::Kind::I32, static_cast<ca::i32>(sem.num), 0};
        return ProducerOutcome::Ok;
    }
    case Op::ACONST_NULL:
        out         = ConstValue{ConstValue::Kind::Null, 0, 0};
        producedRef = true;
        return ProducerOutcome::Ok;
    case Op::GETSTATIC: {
        ir::ClassSession::MemberSem enc;
        if (!session.memberOf(in, enc)) return ProducerOutcome::NotConstant;
        const char prim = knowledge::primitiveTypeDesc(enc.owner);
        if (enc.name != "TYPE" || enc.desc != "Ljava/lang/Class;" || prim == '\0') {
            return ProducerOutcome::UnsupportedGetstatic;
        }
        out         = ConstValue{ConstValue::Kind::PrimClass, 0, prim};
        producedRef = true;
        return ProducerOutcome::Ok;
    }
    default: return ProducerOutcome::NotConstant;
    }
}

/// @brief 容器内全部「单常量方法」定义表：owner\nname\ndesc -> 值
///        （键复合收口在 scan_toolbox::keyOf）。
using ConstMethodIndex = std::unordered_map<std::string, ConstValue>;

/// @brief 形状是否值得进拒绝面：方法体以 ireturn/areturn 结尾的两指令方法。
///        沾边但不成立的才记拒绝（trace 有用）；其余静默跳过，不刷屏。
bool shapePlausible(const ir::Insn& ret) noexcept
{
    return ret.op == Op::IRETURN || ret.op == Op::ARETURN;
}

void buildDefs(const mjbase::model::ResourceContainer& container, ConstMethodIndex& defs,
               std::vector<RejectedMethod>& rejects)
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

            const std::vector<ir::InsnId>& order = cu->dom.order();
            if (order.size() != 2) continue;   ///< 不是两指令形状：静默跳过
            const ir::Insn* producer = cu->dom.peek(order[0]);
            const ir::Insn* ret      = cu->dom.peek(order[1]);
            if (producer == nullptr || ret == nullptr) continue;
            if (!shapePlausible(*ret)) continue;

            const auto reject = [&](RejectReason r) {
                rejects.push_back(RejectedMethod{std::string(s.backing().thisName()),
                                                 std::string(name), std::string(desc), r});
            };

            ConstValue      value;
            bool            producedRef = false;
            ProducerOutcome outcome     = decodeProducer(s, *producer, value, producedRef);
            if (outcome == ProducerOutcome::NotConstant) continue;   ///< 非常量生产：静默
            if (outcome == ProducerOutcome::UnsupportedGetstatic) {
                reject(RejectReason::GetstaticUnsupported);
                continue;
            }
            const bool retMatches =
                (ret->op == Op::IRETURN && !producedRef) ||
                (ret->op == Op::ARETURN && producedRef);
            if (!retMatches) {
                reject(RejectReason::BodyNotSingleConst);
                continue;
            }
            if ((mu.access & kAccSynchronized) != 0) {
                reject(RejectReason::TargetSynchronized);
                continue;
            }
            if (!clinitPure(s)) {
                reject(RejectReason::OwnerClassInitImpure);
                continue;
            }
            defs.emplace(scan_tools::keyOf(s.backing().thisName(), name, desc), value);
        }
    });
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// @brief 按值造调用点替换指令（Class 常量在调用方池内现 intern）。
bool makeInlineLoad(ir::ClassSession& session, const ConstValue& value, ir::Insn& out)
{
    switch (value.kind) {
    case ConstValue::Kind::I32: {
        jsim::SimValue v = jsim::SimValue::knownI32(value.i32);
        return rewrite_tools::makeConstLoad(session, v, out);
    }
    case ConstValue::Kind::Null:
        out = ir::Insn{};
        out.op = Op::ACONST_NULL;
        return true;
    case ConstValue::Kind::PrimClass: {
        const std::string name(1, value.prim);
        const ca::u32 nameCp = session.backing().cp.addUtf8(name);
        if (nameCp == 0) return false;
        const ca::u32 clsCp = session.backing().cp.addClass(nameCp);
        if (clsCp == 0) return false;
        out = ir::CodeEditor::loadConst(clsCp);
        return true;
    }
    default: return false;
    }
}

/// 改写失败原因（trace 用）。
enum class RwResult : ca::u8 {
    Ok = 0,
    CallNotFound,
    MemberMismatch,
    LoadFail,
    SlotFail,
};

/// 在已 promote 的调用者方法里原地替换一个调用点；失败不改动 DOM。
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

    ir::Insn load;
    if (!makeInlineLoad(session, site.value, load)) return RwResult::LoadFail;

    // 原地改写：调用指令的身份与 srcPc 不变，锚在它上面的标签因此不必迁移，
    // 也不需要 constant_call 那套「区间中段标签门」——这里没有删除，没有区间。
    auto slotR = dom.get(order[callPos]);
    if (slotR.is_err()) return RwResult::SlotFail;
    ir::Insn*     insn   = slotR.unwrap();
    const ca::u32 keepPc = insn->srcPc;
    *insn                = load;
    insn->srcPc          = keepPc;
    return RwResult::Ok;
}

}   // namespace

// ============================================================================
// ==== 对外三段 ==============================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budgetTemplate)
{
    ScanResult result;
    ca::usize  named = 0;

    ConstMethodIndex defs;
    buildDefs(container, defs, result.rejects);
    if (defs.empty()) return result;

    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置记账
        ir::ClassSession&     s    = *session;
        const ir::ClassUnit&  unit = s.unit();
        const std::string_view self = s.backing().thisName();

        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            const std::string_view name = unit.symbols().str(unit.methods[mi].name);
            const std::string_view desc = unit.symbols().str(unit.methods[mi].desc);
            const std::vector<ir::InsnId>& order = cu->dom.order();

            for (ca::usize pos = 0; pos < order.size(); ++pos) {
                if (!budget.tick()) {   ///< 逐位置记账：超限即停，类保守跳过
                    ++result.classesSkippedByBudget;
                    iter_budget::reportExhausted("grunt.ConstMethod", "scan",
                                                 std::string(entry.uri), budget, named);
                    return;
                }
                const ir::Insn* in = cu->dom.peek(order[pos]);
                if (in == nullptr || in->op != Op::INVOKESTATIC) continue;

                ir::ClassSession::MemberSem enc;
                if (!s.memberOf(*in, enc)) continue;
                if (enc.kind != ir::ClassSession::MemberSem::Kind::Method) continue;

                const auto it = defs.find(scan_tools::keyOf(enc.owner, enc.name, enc.desc));
                if (it == defs.end()) continue;

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
                site.value             = it->second;
                result.sites.push_back(std::move(site));
            }
        }
    });
    return result;
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budgetTemplate)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& s : result.sites) byUri[s.callerUri].push_back(&s);

    ca::usize named = 0;

    // 两遍式：上面只收动作清单，这里逐类「开会话 → 改写 → lower → 立即回写」，
    // 峰值内存是单类会话而不是整 jar 的改写字节（同 flags_strip 的理由）。
    for (auto& [uri, sites] : byUri) {
        const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
        if (bytes == nullptr) continue;
        auto session = openSession(*bytes);
        if (!session) continue;
        ir::ClassSession& s = *session;

        std::unordered_map<ca::usize, std::vector<const Site*>> byMethod;
        for (const Site* site : sites) byMethod[site->callerMethodIndex].push_back(site);

        std::vector<ca::usize> candidates;
        candidates.reserve(byMethod.size());
        for (const auto& slot : byMethod) candidates.push_back(slot.first);
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(*bytes, candidates);

        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置记账

        bool changed = false;
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

            const ca::usize orderSize = cu->dom.order().size();

            bool    methodChanged = false;
            ca::usize tried        = 0;   ///< 本方法已尝试的站点数
            for (const Site* site : methodSites) {
                if (!budget.tick(orderSize)) {   ///< 逐站点按指令数记账：超限弃余
                    dirty.skippedByBudget += methodSites.size() - tried;
                    iter_budget::reportExhausted("grunt.ConstMethod", "rewrite", uri,
                                                 budget, named);
                    abandoned = true;   ///< 余下方法不再尝试；已完成站点保留
                    break;
                }
                ++tried;
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
            if (abandoned) break;
            if (!methodChanged) continue;
            auto lowered = ir::lowerMethod(s, methodIndex);
            if (lowered.is_err()) {
                if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                    std::fprintf(stderr, "[inline-lower-fail] %s method#%zu err=%s\n",
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

}   // namespace grunt::const_method
