/// @file        const_args.hpp
/// @brief       SCAN 工具箱：纯常量装载识别与实参区收集。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。constant_call / predicate_fold
/// 等残留共用「紧邻纯常量装载」这一区域判定；constant_call.cpp 内的同形私有
/// 副本待迁移收敛（挂账）。collectArgs 跳过 NOP：本管线 squeeze 先行，蹦床
/// NOP 化后遗留落点不驻栈，跳过即可让被压住的操作数区重新可见。
#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/rewrite_toolbox.hpp"
#include "mjbase/asm/evaluator/value.hpp"

namespace grunt::const_args {

namespace ev = mjbase::asm_evaluator;
namespace ir = jbc::ir;
using jbc::core::Op;

/// @brief 纯常量装载识别：iconst/bipush/sipush/ldc 族 + aconst_null + l/f/dconst。
///
/// String/Class/MethodType/MethodHandle/Dynamic 的引用内容不可静态确定，
/// 一律拒绝。`slots` 是压栈槽数（long/double 双槽）。
inline bool decodePush(ir::ClassSession& session, const ir::Insn& in, ev::Value& out,
                       ca::u32& slots)
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
            out   = ev::Value::ofF32(
                rewrite_tools::bitCast<ca::f32>(static_cast<ca::u32>(sem.bits)));
            slots = 1;
            return true;
        case ir::ClassSession::ConstSem::Kind::Double:
            out   = ev::Value::ofF64(rewrite_tools::bitCast<ca::f64>(sem.bits));
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

/// @brief 方法描述符的参数槽数（long/double 双槽）。
inline ca::u32 argSlots(std::string_view desc)
{
    ca::u32   slots = 0;
    ca::usize i     = 1;   // 跳过 '('
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
            // J/D 双槽也按类型码算进槽数。
            i += 1;
            slots += (c == 'J' || c == 'D') ? 2 : 1;
        }
    }
    return slots;
}

/// @brief 从消费点往前收集实参区：每层都是纯常量装载且槽数严格吻合。
///
/// producers 按压栈序（先压在前）；NOP 是 squeeze 改写的遗留落点，不驻栈，
/// 跳过。
inline bool collectArgs(ir::ClassSession& session, const ir::CodeDom& dom,
                        const std::vector<ir::InsnId>& order, ca::usize consumerPos,
                        ca::u32 wantSlots, std::vector<ir::InsnId>& producers,
                        std::vector<ev::Value>& args)
{
    ca::i32                 need = static_cast<ca::i32>(wantSlots);
    ca::usize               j    = consumerPos;
    std::vector<ev::Value>  rev;
    std::vector<ir::InsnId> revIds;
    while (need > 0) {
        if (j == 0) return false;
        --j;
        const ir::Insn* p = dom.peek(order[j]);
        if (p == nullptr) return false;
        if (p->op == Op::NOP) continue;
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

/// @brief ConstantValue 值面的常量载荷（const_value_promote 残留用）。
///
/// ev::Value 装不下字符串，而这个面的判据与栈值面不同（见 decodeValueLoad），
/// 所以单独成结构：kind + 数值/位形 + 字符串。
struct ValueLoad
{
    enum class Kind : ca::u8 { None = 0, I32, I64, F32, F64, Str };

    Kind        kind  = Kind::None;
    ca::u64     bits  = 0;   ///< I32/I64 数值（符号扩展存）；F32/F64 的 IEEE 位形
    std::string str;         ///< Kind::Str
    ca::u32     slots = 1;   ///< J/D 双槽

    ca::i32 i32() const noexcept { return static_cast<ca::i32>(bits); }
    ca::i64 i64() const noexcept { return static_cast<ca::i64>(bits); }
    ca::f32 f32() const noexcept { return rewrite_tools::bitCast<ca::f32>(static_cast<ca::u32>(bits)); }
    ca::f64 f64() const noexcept { return rewrite_tools::bitCast<ca::f64>(bits); }
};

/// @brief ConstantValue 值面的常量装载识别。
///
/// 与 `decodePush` 的栈值面是两个事实：这里接受 LDC String（ConstantValue
/// 有字符串形态），拒绝 ACONST_NULL（ConstantValue 无 null 形态）。
inline bool decodeValueLoad(ir::ClassSession& session, const ir::Insn& in, ValueLoad& out)
{
    const auto intLike = [&](ValueLoad::Kind k, ca::u64 bits, ca::u32 slots) {
        out.kind  = k;
        out.bits  = bits;
        out.slots = slots;
        return true;
    };
    switch (in.op) {
    case Op::ICONST_M1: return intLike(ValueLoad::Kind::I32, static_cast<ca::u64>(static_cast<ca::i64>(-1)), 1);
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
        return intLike(ValueLoad::Kind::I32,
                       static_cast<ca::u64>(static_cast<ca::i64>(static_cast<ca::i32>(in.op)
                                                                 - static_cast<ca::i32>(Op::ICONST_0))),
                       1);
    case Op::BIPUSH:
    case Op::SIPUSH: return intLike(ValueLoad::Kind::I32, static_cast<ca::u64>(static_cast<ca::i64>(in.a)), 1);
    case Op::LCONST_0:
    case Op::LCONST_1:
        return intLike(ValueLoad::Kind::I64,
                       static_cast<ca::u64>(in.op == Op::LCONST_1 ? 1 : 0), 2);
    case Op::FCONST_0:
    case Op::FCONST_1:
    case Op::FCONST_2:
        return intLike(ValueLoad::Kind::F32,
                       rewrite_tools::bitCast<ca::u32>(static_cast<ca::f32>(
                           static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::FCONST_0))),
                       1);
    case Op::DCONST_0:
    case Op::DCONST_1:
        return intLike(ValueLoad::Kind::F64,
                       rewrite_tools::bitCast<ca::u64>(static_cast<ca::f64>(
                           static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::DCONST_0))),
                       2);
    case Op::LDC:
    case Op::LDC_W:
    case Op::LDC2_W: {
        ir::ClassSession::ConstSem sem;
        if (!session.constOf(in, sem)) return false;
        using K = ir::ClassSession::ConstSem::Kind;
        switch (sem.kind) {
        case K::Int: return intLike(ValueLoad::Kind::I32, static_cast<ca::u64>(static_cast<ca::i64>(static_cast<ca::i32>(sem.num))), 1);
        case K::Long: return intLike(ValueLoad::Kind::I64, static_cast<ca::u64>(sem.num), 2);
        case K::Float: return intLike(ValueLoad::Kind::F32, static_cast<ca::u64>(sem.bits), 1);
        case K::Double: return intLike(ValueLoad::Kind::F64, sem.bits, 2);
        case K::String:
            out.kind = ValueLoad::Kind::Str;
            out.str.assign(sem.str);
            out.slots = 1;
            return true;
        default: return false;
        }
    }
    default: return false;
    }
}

/// @brief 求值值的同一性（同类别按位相同；null 引用互相等同）。
inline bool sameValue(const ev::Value& a, const ev::Value& b)
{
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case ev::Value::Kind::I32: return a.i32 == b.i32;
    case ev::Value::Kind::I64: return a.i64 == b.i64;
    case ev::Value::Kind::F32:
        return rewrite_tools::bitCast<ca::u32>(a.f32)
               == rewrite_tools::bitCast<ca::u32>(b.f32);
    case ev::Value::Kind::F64:
        return rewrite_tools::bitCast<ca::u64>(a.f64)
               == rewrite_tools::bitCast<ca::u64>(b.f64);
    case ev::Value::Kind::Ref: return a.isNull() && b.isNull();
    default: return false;
    }
}

}   // namespace grunt::const_args
