/// @file        constant_call.cpp
/// @brief       残留 `constant_call` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/constant_call.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "mjbase/asm/evaluator/evaluator.hpp"
#include "mjbase/asm/evaluator/method_sig.hpp"
#include "mjbase/asm/insn/insn_factory.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::constant_call {

namespace {

namespace ev = mjbase::asm_evaluator;
namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

template <class To, class From>
To bitCast(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bitCast size mismatch");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// 粗找：常量装载指令族。只钉形状（op 族籍），值由细找经常量池解析。
bool isConstPush(Op op) noexcept
{
    switch (op) {
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
    case Op::LDC:
    case Op::LDC_W:
    case Op::LDC2_W: return true;
    default: return false;
    }
}

/// 细找：把一条常量装载指令解成具体值 + 占用槽数。
/// 引用常量只接受 `aconst_null`（String/Class 等不猜内容）。
bool decodePush(ir::ClassSession& session, const ir::Insn& in, ev::Value& out, ca::u32& slots)
{
    const auto i32At = [&](ca::i32 v) {
        out   = ev::Value::ofI32(v);
        slots = 1;
        return true;
    };
    switch (in.op) {
    case Op::ACONST_NULL:
        out   = ev::Value::ofNull();
        slots = 1;
        return true;
    case Op::ICONST_M1:
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
        return i32At(static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::ICONST_0));
    case Op::BIPUSH:
    case Op::SIPUSH: return i32At(in.a);
    case Op::LCONST_0:
    case Op::LCONST_1:
        out   = ev::Value::ofI64(in.op == Op::LCONST_1 ? 1 : 0);
        slots = 2;
        return true;
    case Op::FCONST_0:
    case Op::FCONST_1:
    case Op::FCONST_2:
        out   = ev::Value::ofF32(static_cast<ca::f32>(static_cast<ca::i32>(in.op) -
                                                     static_cast<ca::i32>(Op::FCONST_0)));
        slots = 1;
        return true;
    case Op::DCONST_0:
    case Op::DCONST_1:
        out   = ev::Value::ofF64(static_cast<ca::f64>(static_cast<ca::i32>(in.op) -
                                                     static_cast<ca::i32>(Op::DCONST_0)));
        slots = 2;
        return true;
    case Op::LDC:
    case Op::LDC_W:
    case Op::LDC2_W: {
        ir::ClassSession::ConstSem sem;
        if (!session.constOf(in, sem)) return false;
        switch (sem.kind) {
        case ir::ClassSession::ConstSem::Kind::Int:
            out   = ev::Value::ofI32(static_cast<ca::i32>(sem.num));
            slots = 1;
            return true;
        case ir::ClassSession::ConstSem::Kind::Long:
            out   = ev::Value::ofI64(sem.num);
            slots = 2;
            return true;
        case ir::ClassSession::ConstSem::Kind::Float:
            out   = ev::Value::ofF32(bitCast<ca::f32>(static_cast<ca::u32>(sem.bits)));
            slots = 1;
            return true;
        case ir::ClassSession::ConstSem::Kind::Double:
            out   = ev::Value::ofF64(bitCast<ca::f64>(sem.bits));
            slots = 2;
            return true;
        default:
            // String / Class / MethodType / MethodHandle / Dynamic：
            // 引用内容不可静态确定，拒绝该站点。
            return false;
        }
    }
    default: return false;
    }
}

/// 方法描述符的参数槽数（long/double 双槽）。
ca::u32 argSlots(std::string_view desc)
{
    ca::u32   slots = 0;
    ca::usize i     = 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        if (c == 'L') {
            const ca::usize semi = desc.find(';', i);
            if (semi == std::string_view::npos) return 0;
            i = semi + 1;
            ++slots;
        } else if (c == '[') {
            while (i < desc.size() && desc[i] == '[') ++i;
            if (i >= desc.size()) return 0;
            if (desc[i] == 'L') {
                const ca::usize semi = desc.find(';', i);
                if (semi == std::string_view::npos) return 0;
                i = semi + 1;
            } else {
                ++i;
            }
            ++slots;
        } else {
            slots += (c == 'J' || c == 'D') ? 2 : 1;
            ++i;
        }
    }
    return slots;
}

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

/// 从调用点往前收集实参；要求每一层都是纯常量装载且槽数严格吻合。
bool collectArgs(ir::ClassSession& session, const ir::CodeDom& dom,
                 const std::vector<ir::InsnId>& order, ca::usize callPos, ca::u32 wantSlots,
                 std::vector<ir::InsnId>& producers, std::vector<ev::Value>& args)
{
    ca::i32                 need = static_cast<ca::i32>(wantSlots);
    ca::usize               j    = callPos;
    std::vector<ev::Value>  rev;
    std::vector<ir::InsnId> revIds;
    while (need > 0) {
        if (j == 0) return false;
        --j;
        const ir::Insn* p = dom.peek(order[j]);
        if (p == nullptr) return false;
        ev::Value v;
        ca::u32   s = 0;
        if (!decodePush(session, *p, v, s)) return false;
        rev.push_back(v);
        revIds.push_back(order[j]);
        need -= static_cast<ca::i32>(s);
    }
    if (need != 0) return false;
    std::reverse(rev.begin(), rev.end());
    std::reverse(revIds.begin(), revIds.end());
    args      = std::move(rev);
    producers = std::move(revIds);
    return true;
}

RejectReason mapEvalError(ev::EvalError::Kind kind)
{
    switch (kind) {
    case ev::EvalError::Kind::Unsupported: return RejectReason::EvalUnsupported;
    case ev::EvalError::Kind::StepLimit: return RejectReason::EvalStepLimit;
    case ev::EvalError::Kind::BadState:
    default: return RejectReason::EvalBadState;
    }
}

/// 求值段：按目标类分组，逐类开会话、判类初始化、跑具体求值栈机。
/// 通过者写回 `value`；失败者转成拒绝记录。
void evaluateSites(const mjbase::model::ResourceContainer& container,
                   std::vector<Site>& sites, std::vector<RejectedSite>& rejects)
{
    std::unordered_map<std::string, std::vector<ca::usize>> byOwner;
    for (ca::usize i = 0; i < sites.size(); ++i) byOwner[sites[i].targetOwner].push_back(i);

    std::vector<ca::u8> ok(sites.size(), 0);   // 0 待判 / 1 通过 / 2 拒绝

    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession&     s    = *session;
        const std::string_view self = s.backing().thisName();

        const auto group = byOwner.find(std::string(self));
        if (group == byOwner.end()) return;

        if (!classInitNeutral(s)) {
            for (const ca::usize si : group->second) {
                rejects.push_back(RejectedSite{sites[si].callerOwner, sites[si].callerMethod,
                                               sites[si].callSrcPc,
                                               RejectReason::ClassInitPresent});
            }
            return;
        }

        std::unordered_map<ca::usize, ir::CodeDom*> doms;
        for (const ca::usize si : group->second) {
            Site& site = sites[si];

            if (!ir::hasCode(s, site.targetMethodIndex)) {
                rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod, site.callSrcPc,
                                               RejectReason::TargetNoCode});
                continue;
            }

            ir::CodeDom* dom = nullptr;
            const auto   dit = doms.find(site.targetMethodIndex);
            if (dit != doms.end()) {
                dom = dit->second;
            } else {
                auto promoted = ir::promoteMethod(s, site.targetMethodIndex);
                if (promoted.is_err()) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetNoCode});
                    continue;
                }
                ir::CodeUnit* cu = promoted.unwrap().get();
                if (cu == nullptr) continue;
                dom = &cu->dom;
                doms.emplace(site.targetMethodIndex, dom);
            }

            auto sig = ev::MethodSig::parse(site.targetDesc, /*isStatic=*/true);
            if (sig.is_err()) {
                rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod, site.callSrcPc,
                                               RejectReason::EvalBadState});
                continue;
            }

            ev::Options options;
            options.session = &s;
            auto outcome    = ev::evaluate(*dom, sig.unwrap(), site.args, options);
            if (outcome.is_err()) {
                rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod, site.callSrcPc,
                                               mapEvalError(outcome.unwrap_err().kind)});
                continue;
            }
            const ev::EvalOutcome oc = outcome.unwrap();
            if (oc.kind != ev::EvalOutcome::Kind::Value) {
                rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod, site.callSrcPc,
                                               RejectReason::TargetVoid});
                continue;
            }
            if (oc.value.kind == ev::Value::Kind::Ref || oc.value.kind == ev::Value::Kind::Top) {
                rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod, site.callSrcPc,
                                               RejectReason::ValueKindUnsupported});
                continue;
            }
            site.value = oc.value;
            ok[si]     = 1;
        }
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

bool sameValue(const ev::Value& a, const ev::Value& b)
{
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case ev::Value::Kind::I32: return a.i32 == b.i32;
    case ev::Value::Kind::I64: return a.i64 == b.i64;
    case ev::Value::Kind::F32: return bitCast<ca::u32>(a.f32) == bitCast<ca::u32>(b.f32);
    case ev::Value::Kind::F64: return bitCast<ca::u64>(a.f64) == bitCast<ca::u64>(b.f64);
    case ev::Value::Kind::Ref: return a.isNull() && b.isNull();
    default: return false;
    }
}

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

    std::vector<ir::InsnId> producers;
    std::vector<ev::Value>  args;
    if (!collectArgs(session, dom, order, callPos, argSlots(enc.desc), producers, args)) {
        return RwResult::ArgsMismatch;
    }
    if (args.size() != site.args.size()) return RwResult::ArgsMismatch;
    for (ca::usize i = 0; i < args.size(); ++i) {
        if (!sameValue(args[i], site.args[i])) return RwResult::ArgsMismatch;
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

                std::vector<ir::InsnId> producers;
                std::vector<ev::Value>  args;
                if (!collectArgs(s, cu->dom, order, pos, argSlots(enc.desc), producers, args)) {
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
                        for (const ev::Value& arg : site->args) {
                            switch (arg.kind) {
                            case ev::Value::Kind::I32: std::fprintf(stderr, "%d ", arg.i32); break;
                            case ev::Value::Kind::I64: std::fprintf(stderr, "%lld ", (long long)arg.i64); break;
                            case ev::Value::Kind::Ref: std::fprintf(stderr, "null "); break;
                            default: std::fprintf(stderr, "? "); break;
                            }
                        }
                        switch (site->value.kind) {
                        case ev::Value::Kind::I32: std::fprintf(stderr, "=> %d\n", site->value.i32); break;
                        case ev::Value::Kind::I64:
                            std::fprintf(stderr, "=> %lld\n", (long long)site->value.i64);
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
