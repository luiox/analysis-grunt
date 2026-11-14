/// @file        arith_fold.cpp
/// @brief       残留 `arith_fold` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 数据流
///
/// `Site{classIdx, methodIndex, anchorSrcPc, anchorOp, shapeIdx|255, SiteValue}`
/// → `DirtySet{uris, sites, attempted, failed, skippedByRoundTrip, skippedByBudget}`。
///
/// ## 求值纪律
///
/// 常量窗口的求值器是 JVM 精确语义：int/long 运算在无符号位形上回绕，
/// 移位计数按 5/6 位掩码，IDIV/IREM 的 MIN/-1 特例（商 MIN、余 0，JVM 定义
/// 值非异常）显式处理，除数 0 整站停窗（保留原地异常），浮点按 IEEE
/// 运算逐位复算，F2I/D2I 族 NaN 归零 + 饱和钳位。任何一步类型不吻合
/// （IADD 见到 I64 槽）即停窗，不猜。
#include "grunt/arith_fold.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/iter_budget.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

#include "jsim/value.hpp"

namespace grunt::arith_fold {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using const_args::ValueLoad;
using proof_tools::roundTripSafe;

// ============================================================================
// ==== 替换形状表（v2/v3 生成源逐字相同，八式全代数恒等） ====================
// ============================================================================

struct ShapeRule
{
    std::vector<Op> seq;     ///< 窗口指令序列（含锚点）
    Op              target;  ///< 折回的单条指令
};

const std::vector<ShapeRule>& shapeRules()
{
    static const std::vector<ShapeRule> rules = {
        {{Op::ICONST_M1, Op::IXOR, Op::ICONST_1, Op::IADD}, Op::INEG},
        {{Op::ICONST_1, Op::ISUB, Op::ICONST_M1, Op::IXOR}, Op::INEG},
        {{Op::SWAP, Op::DUP_X1, Op::ICONST_M1, Op::IXOR, Op::IOR, Op::SWAP,
          Op::ICONST_M1, Op::IXOR, Op::ISUB},
         Op::IAND},
        {{Op::DUP_X1, Op::ICONST_M1, Op::IXOR, Op::IAND, Op::IADD}, Op::IOR},
        {{Op::DUP2, Op::IOR, Op::DUP_X2, Op::POP, Op::IAND, Op::ISUB}, Op::IXOR},
        {{Op::DUP2, Op::ICONST_M1, Op::IXOR, Op::IAND, Op::DUP_X2, Op::POP,
          Op::SWAP, Op::ICONST_M1, Op::IXOR, Op::IAND, Op::IOR},
         Op::IXOR},
        {{Op::DUP2, Op::IOR, Op::DUP_X2, Op::POP, Op::ICONST_M1, Op::IXOR,
          Op::SWAP, Op::ICONST_M1, Op::IXOR, Op::IOR, Op::IAND},
         Op::IXOR},
        {{Op::DUP2, Op::IOR, Op::DUP_X2, Op::POP, Op::IAND, Op::ICONST_M1,
          Op::IXOR, Op::IAND},
         Op::IXOR},
    };
    return rules;
}

// ============================================================================
// ==== 常量窗口求值器（JVM 精确语义） =======================================
// ============================================================================

/// 窗口内一个逻辑栈槽：值 + 参与指令的位置位掩码（窗口非 NOP 序号）。
struct Slot
{
    ValueLoad v;
    ca::u32   mask = 0;
};

constexpr ca::usize kMaxWindow = 16;   ///< 窗口非 NOP 指令上限

/// @brief 常量装载是否可作窗口锚（数值四形；字符串/引用形不进窗口）。
bool numericLoad(ir::ClassSession& s, const ir::Insn& in, ValueLoad& out)
{
    if (!const_args::decodeValueLoad(s, in, out)) return false;
    return out.kind == ValueLoad::Kind::I32 || out.kind == ValueLoad::Kind::I64
           || out.kind == ValueLoad::Kind::F32 || out.kind == ValueLoad::Kind::F64;
}

ca::i32 shlI32(ca::i32 x, ca::i32 s) noexcept
{
    const ca::u32 n = static_cast<ca::u32>(s) & 31u;
    return static_cast<ca::i32>(static_cast<ca::u32>(x) << n);
}

ca::i32 shrI32(ca::i32 x, ca::i32 s) noexcept
{
    const ca::u32 n = static_cast<ca::u32>(s) & 31u;
    if (n == 0) return x;
    const ca::u32 u = static_cast<ca::u32>(x);
    if (u & 0x80000000u) {
        return static_cast<ca::i32>((u >> n) | (0xFFFFFFFFu << (32u - n)));
    }
    return static_cast<ca::i32>(u >> n);
}

ca::i32 ushrI32(ca::i32 x, ca::i32 s) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(x) >> (static_cast<ca::u32>(s) & 31u));
}

ca::i64 shlI64(ca::i64 x, ca::i32 s) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(x) << (static_cast<ca::u64>(s) & 63u));
}

ca::i64 shrI64(ca::i64 x, ca::i32 s) noexcept
{
    const ca::u64 n = static_cast<ca::u64>(s) & 63u;
    if (n == 0) return x;
    const ca::u64 u = static_cast<ca::u64>(x);
    if (u & 0x8000000000000000ull) {
        return static_cast<ca::i64>((u >> n) | (0xFFFFFFFFFFFFFFFFull << (64u - n)));
    }
    return static_cast<ca::i64>(u >> n);
}

ca::i64 ushrI64(ca::i64 x, ca::i32 s) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(x) >> (static_cast<ca::u64>(s) & 63u));
}

/// JVM i32 除法（MIN/-1 商 MIN，无异常）；除数 0 返回 false。
bool divI32(ca::i32 x, ca::i32 y, ca::i32& out) noexcept
{
    if (y == 0) return false;
    if (x == INT32_MIN && y == -1) {
        out = INT32_MIN;
        return true;
    }
    out = x / y;
    return true;
}

/// JVM i32 取余（MIN%-1 余 0）；除数 0 返回 false。
bool remI32(ca::i32 x, ca::i32 y, ca::i32& out) noexcept
{
    if (y == 0) return false;
    if (x == INT32_MIN && y == -1) {
        out = 0;
        return true;
    }
    out = x % y;
    return true;
}

bool divI64(ca::i64 x, ca::i64 y, ca::i64& out) noexcept
{
    if (y == 0) return false;
    if (x == INT64_MIN && y == -1) {
        out = INT64_MIN;
        return true;
    }
    out = x / y;
    return true;
}

bool remI64(ca::i64 x, ca::i64 y, ca::i64& out) noexcept
{
    if (y == 0) return false;
    if (x == INT64_MIN && y == -1) {
        out = 0;
        return true;
    }
    out = x % y;
    return true;
}

/// JVM f2i / d2i：NaN 归 0，正负饱和到 int 边界。
ca::i32 floatToI32(ca::f64 v) noexcept
{
    if (v != v) return 0;
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return static_cast<ca::i32>(v);
}

/// JVM f2l / d2l：NaN 归 0，正负饱和到 long 边界。
ca::i64 floatToI64(ca::f64 v) noexcept
{
    if (v != v) return 0;
    if (v >= 9223372036854775807.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<ca::i64>(v);
}

ValueLoad makeI32(ca::i32 v) noexcept
{
    ValueLoad out;
    out.kind  = ValueLoad::Kind::I32;
    out.bits  = static_cast<ca::u64>(static_cast<ca::i64>(v));
    out.slots = 1;
    return out;
}

ValueLoad makeI64(ca::i64 v) noexcept
{
    ValueLoad out;
    out.kind  = ValueLoad::Kind::I64;
    out.bits  = static_cast<ca::u64>(v);
    out.slots = 2;
    return out;
}

ValueLoad makeF32(ca::f32 v) noexcept
{
    ValueLoad out;
    out.kind  = ValueLoad::Kind::F32;
    out.bits  = static_cast<ca::u64>(grunt::rewrite_tools::bitCast<ca::u32>(v));
    out.slots = 1;
    return out;
}

ValueLoad makeF64(ca::f64 v) noexcept
{
    ValueLoad out;
    out.kind  = ValueLoad::Kind::F64;
    out.bits  = grunt::rewrite_tools::bitCast<ca::u64>(v);
    out.slots = 2;
    return out;
}

/// @brief 常量窗口单步求值。@return false = 该指令不可求值，停窗。
///
/// `stack` 是窗口内逻辑槽（bottom→top）；`idx` 是该指令的窗口序号。
/// 白名单两族（hpp 判据 2）：纯常量装载（压新槽，锚点同族）与封闭数值
/// 运算。求值成功时消费操作数槽并压结果槽；类型不吻合返回 false（停窗）。
bool evalStep(ir::ClassSession& s, const ir::Insn& in, ca::usize idx,
              std::vector<Slot>& stack)
{
    const Op      op      = in.op;
    const ca::u32 selfBit = 1u << idx;
    using K               = ValueLoad::Kind;

    {
        ValueLoad v;
        if (numericLoad(s, in, v)) {
            stack.push_back(Slot{v, selfBit});
            return true;
        }
    }

    const auto pop = [&](Slot& out) {
        if (stack.empty()) return false;
        out = stack.back();
        stack.pop_back();
        return true;
    };

    // ---- 一元 ----
    if (op == Op::INEG) {
        Slot a;
        if (!pop(a) || a.v.kind != K::I32) return false;
        stack.push_back(Slot{makeI32(-a.v.i32()), a.mask | selfBit});
        return true;
    }
    if (op == Op::LNEG) {
        Slot a;
        if (!pop(a) || a.v.kind != K::I64) return false;
        stack.push_back(Slot{makeI64(-a.v.i64()), a.mask | selfBit});
        return true;
    }
    if (op == Op::FNEG) {
        Slot a;
        if (!pop(a) || a.v.kind != K::F32) return false;
        const ca::u32 bits = static_cast<ca::u32>(a.v.bits) ^ 0x80000000u;
        ValueLoad r;
        r.kind = K::F32; r.bits = bits; r.slots = 1;
        stack.push_back(Slot{r, a.mask | selfBit});
        return true;
    }
    if (op == Op::DNEG) {
        Slot a;
        if (!pop(a) || a.v.kind != K::F64) return false;
        const ca::u64 bits = a.v.bits ^ 0x8000000000000000ull;
        ValueLoad r;
        r.kind = K::F64; r.bits = bits; r.slots = 2;
        stack.push_back(Slot{r, a.mask | selfBit});
        return true;
    }

    // ---- int 二元 ----
    const auto i32Bin = [&](ca::i32 (*fn)(ca::i32, ca::i32)) {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I32 || b.v.kind != K::I32) return false;
        stack.push_back(Slot{makeI32(fn(a.v.i32(), b.v.i32())), a.mask | b.mask | selfBit});
        return true;
    };
    switch (op) {
    case Op::IADD: return i32Bin([](ca::i32 x, ca::i32 y) {
        return static_cast<ca::i32>(static_cast<ca::u32>(x) + static_cast<ca::u32>(y));
    });
    case Op::ISUB: return i32Bin([](ca::i32 x, ca::i32 y) {
        return static_cast<ca::i32>(static_cast<ca::u32>(x) - static_cast<ca::u32>(y));
    });
    case Op::IMUL: return i32Bin([](ca::i32 x, ca::i32 y) {
        return static_cast<ca::i32>(static_cast<ca::u32>(x) * static_cast<ca::u32>(y));
    });
    case Op::IAND: return i32Bin([](ca::i32 x, ca::i32 y) {
        return static_cast<ca::i32>(static_cast<ca::u32>(x) & static_cast<ca::u32>(y));
    });
    case Op::IOR: return i32Bin([](ca::i32 x, ca::i32 y) {
        return static_cast<ca::i32>(static_cast<ca::u32>(x) | static_cast<ca::u32>(y));
    });
    case Op::IXOR: return i32Bin([](ca::i32 x, ca::i32 y) {
        return static_cast<ca::i32>(static_cast<ca::u32>(x) ^ static_cast<ca::u32>(y));
    });
    case Op::ISHL: return i32Bin([](ca::i32 x, ca::i32 y) { return shlI32(x, y); });
    case Op::ISHR: return i32Bin([](ca::i32 x, ca::i32 y) { return shrI32(x, y); });
    case Op::IUSHR: return i32Bin([](ca::i32 x, ca::i32 y) { return ushrI32(x, y); });
    default: break;
    }
    if (op == Op::IDIV || op == Op::IREM) {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I32 || b.v.kind != K::I32) return false;
        ca::i32 r = 0;
        if (op == Op::IDIV) {
            if (!divI32(a.v.i32(), b.v.i32(), r)) return false;
        } else if (!remI32(a.v.i32(), b.v.i32(), r)) {
            return false;
        }
        stack.push_back(Slot{makeI32(r), a.mask | b.mask | selfBit});
        return true;
    }

    // ---- long 二元（移位第二操作数是 int） ----
    const auto i64Bin = [&](ca::i64 (*fn)(ca::i64, ca::i64)) {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I64 || b.v.kind != K::I64) return false;
        stack.push_back(Slot{makeI64(fn(a.v.i64(), b.v.i64())), a.mask | b.mask | selfBit});
        return true;
    };
    switch (op) {
    case Op::LADD: return i64Bin([](ca::i64 x, ca::i64 y) {
        return static_cast<ca::i64>(static_cast<ca::u64>(x) + static_cast<ca::u64>(y));
    });
    case Op::LSUB: return i64Bin([](ca::i64 x, ca::i64 y) {
        return static_cast<ca::i64>(static_cast<ca::u64>(x) - static_cast<ca::u64>(y));
    });
    case Op::LMUL: return i64Bin([](ca::i64 x, ca::i64 y) {
        return static_cast<ca::i64>(static_cast<ca::u64>(x) * static_cast<ca::u64>(y));
    });
    case Op::LAND: return i64Bin([](ca::i64 x, ca::i64 y) {
        return static_cast<ca::i64>(static_cast<ca::u64>(x) & static_cast<ca::u64>(y));
    });
    case Op::LOR: return i64Bin([](ca::i64 x, ca::i64 y) {
        return static_cast<ca::i64>(static_cast<ca::u64>(x) | static_cast<ca::u64>(y));
    });
    case Op::LXOR: return i64Bin([](ca::i64 x, ca::i64 y) {
        return static_cast<ca::i64>(static_cast<ca::u64>(x) ^ static_cast<ca::u64>(y));
    });
    case Op::LSHL: {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I64 || b.v.kind != K::I32) return false;
        stack.push_back(Slot{makeI64(shlI64(a.v.i64(), b.v.i32())), a.mask | b.mask | selfBit});
        return true;
    }
    case Op::LSHR: {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I64 || b.v.kind != K::I32) return false;
        stack.push_back(Slot{makeI64(shrI64(a.v.i64(), b.v.i32())), a.mask | b.mask | selfBit});
        return true;
    }
    case Op::LUSHR: {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I64 || b.v.kind != K::I32) return false;
        stack.push_back(Slot{makeI64(ushrI64(a.v.i64(), b.v.i32())), a.mask | b.mask | selfBit});
        return true;
    }
    default: break;
    }
    if (op == Op::LDIV || op == Op::LREM) {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::I64 || b.v.kind != K::I64) return false;
        ca::i64 r = 0;
        if (op == Op::LDIV) {
            if (!divI64(a.v.i64(), b.v.i64(), r)) return false;
        } else if (!remI64(a.v.i64(), b.v.i64(), r)) {
            return false;
        }
        stack.push_back(Slot{makeI64(r), a.mask | b.mask | selfBit});
        return true;
    }

    // ---- float / double 二元 ----
    const auto f32Bin = [&](ca::f32 (*fn)(ca::f32, ca::f32)) {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::F32 || b.v.kind != K::F32) return false;
        stack.push_back(Slot{makeF32(fn(a.v.f32(), b.v.f32())), a.mask | b.mask | selfBit});
        return true;
    };
    switch (op) {
    case Op::FADD: return f32Bin([](ca::f32 x, ca::f32 y) { return x + y; });
    case Op::FSUB: return f32Bin([](ca::f32 x, ca::f32 y) { return x - y; });
    case Op::FMUL: return f32Bin([](ca::f32 x, ca::f32 y) { return x * y; });
    case Op::FDIV: return f32Bin([](ca::f32 x, ca::f32 y) { return x / y; });
    default: break;
    }
    const auto f64Bin = [&](ca::f64 (*fn)(ca::f64, ca::f64)) {
        Slot b, a;
        if (!pop(b) || !pop(a) || a.v.kind != K::F64 || b.v.kind != K::F64) return false;
        stack.push_back(Slot{makeF64(fn(a.v.f64(), b.v.f64())), a.mask | b.mask | selfBit});
        return true;
    };
    switch (op) {
    case Op::DADD: return f64Bin([](ca::f64 x, ca::f64 y) { return x + y; });
    case Op::DSUB: return f64Bin([](ca::f64 x, ca::f64 y) { return x - y; });
    case Op::DMUL: return f64Bin([](ca::f64 x, ca::f64 y) { return x * y; });
    case Op::DDIV: return f64Bin([](ca::f64 x, ca::f64 y) { return x / y; });
    default: break;
    }

    // ---- 数值转换（fn 取 Slot 出值，K 是源类型钉） ----
    const auto conv1 = [&](K from, auto fn) {
        Slot a;
        if (!pop(a) || a.v.kind != from) return false;
        stack.push_back(Slot{fn(a.v), a.mask | selfBit});
        return true;
    };
    switch (op) {
    case Op::I2L: return conv1(K::I32, [](const ValueLoad& v) { return makeI64(v.i32()); });
    case Op::L2I: return conv1(K::I64, [](const ValueLoad& v) {
        return makeI32(static_cast<ca::i32>(static_cast<ca::u32>(static_cast<ca::u64>(v.i64()))));
    });
    case Op::I2F: return conv1(K::I32, [](const ValueLoad& v) { return makeF32(static_cast<ca::f32>(v.i32())); });
    case Op::I2D: return conv1(K::I32, [](const ValueLoad& v) { return makeF64(static_cast<ca::f64>(v.i32())); });
    case Op::L2F: return conv1(K::I64, [](const ValueLoad& v) { return makeF32(static_cast<ca::f32>(v.i64())); });
    case Op::L2D: return conv1(K::I64, [](const ValueLoad& v) { return makeF64(static_cast<ca::f64>(v.i64())); });
    case Op::F2I: return conv1(K::F32, [](const ValueLoad& v) { return makeI32(floatToI32(v.f32())); });
    case Op::F2L: return conv1(K::F32, [](const ValueLoad& v) { return makeI64(floatToI64(v.f32())); });
    case Op::F2D: return conv1(K::F32, [](const ValueLoad& v) { return makeF64(v.f32()); });
    case Op::D2I: return conv1(K::F64, [](const ValueLoad& v) { return makeI32(floatToI32(v.f64())); });
    case Op::D2L: return conv1(K::F64, [](const ValueLoad& v) { return makeI64(floatToI64(v.f64())); });
    case Op::D2F: return conv1(K::F64, [](const ValueLoad& v) { return makeF32(static_cast<ca::f32>(v.f64())); });
    case Op::I2B: return conv1(K::I32, [](const ValueLoad& v) {
        return makeI32(static_cast<ca::i8>(v.i32() & 0xFF));
    });
    case Op::I2C: return conv1(K::I32, [](const ValueLoad& v) { return makeI32(v.i32() & 0xFFFF); });
    case Op::I2S: return conv1(K::I32, [](const ValueLoad& v) {
        return makeI32(static_cast<ca::i16>(v.i32() & 0xFFFF));
    });
    default: break;
    }

    return false;
}

/// @brief 常量窗口匹配：从 anchorPos 起模拟，整窗归一才收。
///
/// @param[out] lastPos  窗口末条（dom order 位置）。
/// @param[out] value    折叠结果。
bool matchConstWindow(ir::ClassSession& s, const ir::CodeDom& dom,
                      const std::vector<ir::InsnId>& order, ca::usize anchorPos,
                      ca::usize& lastPos, SiteValue& value)
{
    std::vector<ca::usize>       pos;
    std::vector<const ir::Insn*> in;
    pos.reserve(kMaxWindow);
    in.reserve(kMaxWindow);
    for (ca::usize p = anchorPos; p < order.size() && in.size() < kMaxWindow; ++p) {
        const ir::Insn* cur = dom.peek(order[p]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        pos.push_back(p);
        in.push_back(cur);
    }

    ValueLoad anchor;
    if (!numericLoad(s, *in[0], anchor)) return false;

    std::vector<Slot> stack;
    stack.push_back(Slot{anchor, 1u << 0});
    ca::usize stop = 1;
    for (ca::usize idx = 1; idx < in.size(); ++idx) {
        if (!evalStep(s, *in[idx], idx, stack)) break;
        stop = idx + 1;
    }
    if (stop < 2) return false;

    // 整窗归一：恰一个逻辑值 + 全部非 NOP 指令参与。
    if (stack.size() != 1) return false;
    ca::u32 covered = 0;
    for (ca::usize i = 0; i < stop; ++i) covered |= 1u << i;
    if (stack[0].mask != covered) return false;

    // 窗口内除锚点外无标签（合并点耦合，放弃）。
    for (ca::usize p = pos[0] + 1; p <= pos[stop - 1] && p < order.size(); ++p) {
        if (!dom.labelsAt(order[p]).empty()) return false;
    }

    lastPos         = pos[stop - 1];
    value.kind      = stack[0].v.kind;
    value.bits      = stack[0].v.bits;
    return true;
}

/// @brief 替换形状匹配：锚点 op 吻合后逐条对齐。
bool matchShape(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                ca::usize anchorPos, ca::u8& shapeIdx, ca::usize& lastPos)
{
    const std::vector<ShapeRule>& rules = shapeRules();
    for (ca::usize r = 0; r < rules.size(); ++r) {
        const ShapeRule& rule = rules[r];
        ca::usize        got  = 0;
        for (ca::usize p = anchorPos; p < order.size() && got < rule.seq.size(); ++p) {
            const ir::Insn* cur = dom.peek(order[p]);
            if (cur == nullptr || cur->op == Op::NOP) continue;
            if (cur->op != rule.seq[got]) break;
            if (got > 0 && !dom.labelsAt(order[p]).empty()) break;   ///< 合并点
            ++got;
            if (got == rule.seq.size()) {
                lastPos  = p;
                shapeIdx = static_cast<ca::u8>(r);
                return true;
            }
        }
    }
    return false;
}

/// @brief 锚点处尝试两类站点。@return 是否命中（span 落 spanLast）。
bool tryMatchAt(ir::ClassSession& s, const ir::CodeDom& dom,
                const std::vector<ir::InsnId>& order, ca::usize anchorPos, ca::u8& shapeIdx,
                ca::usize& spanLast, SiteValue& value)
{
    ca::u8    idx8 = 0;
    ca::usize last = 0;
    if (matchShape(dom, order, anchorPos, idx8, last)) {
        shapeIdx = idx8;
        spanLast = last;
        return true;
    }
    if (matchConstWindow(s, dom, order, anchorPos, last, value)) {
        shapeIdx = 255;
        spanLast = last;
        return true;
    }
    return false;
}

jsim::SimValue simOf(const SiteValue& v)
{
    switch (v.kind) {
    case ValueLoad::Kind::I32: return jsim::SimValue::knownI32(v.i32());
    case ValueLoad::Kind::I64: return jsim::SimValue::knownI64(v.i64());
    case ValueLoad::Kind::F32: return jsim::SimValue::knownF32(v.f32());
    case ValueLoad::Kind::F64: return jsim::SimValue::knownF64(v.f64());
    default: return jsim::SimValue{};
    }
}

/// @brief 单方法 SCAN。@return false = 类级迭代预算超限。
bool scanMethod(ir::ClassSession& s, ca::usize methodIndex, ca::u32 classIdx,
                ScanResult& result, iter_budget::ClassBudget& budget)
{
    if (!ir::hasCode(s, methodIndex)) return true;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return true;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return true;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      pos   = 0;
    while (pos < order.size()) {
        if (!budget.tick()) return false;
        const ir::Insn* in = cu->dom.peek(order[pos]);
        if (in == nullptr || in->op == Op::NOP) {
            ++pos;
            continue;
        }
        Site      site;
        ca::usize spanLast = 0;
        if (tryMatchAt(s, cu->dom, order, pos, site.shapeIdx, spanLast, site.value)) {
            site.classIdx    = classIdx;
            site.methodIndex = methodIndex;
            site.anchorSrcPc = in->srcPc;
            site.anchorOp    = in->op;
            result.sites.push_back(site);
            if (site.shapeIdx == 255) ++result.constSites;
            else ++result.shapeSites;
            pos = spanLast + 1;
            continue;
        }
        ++pos;
    }
    return true;
}

/// @brief 单方法 REWRITE。@return 折叠成功站点数。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ca::usize& failed,
                        ca::usize& skippedByBudget, iter_budget::ClassBudget& budget)
{
    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      hit   = 0;
    ca::usize                      tried = 0;

    for (const Site* site : sites) {
        if (!budget.tick(order.size())) {
            skippedByBudget += sites.size() - tried;
            return hit;
        }
        ++tried;

        ca::usize anchorPos = static_cast<ca::usize>(-1);
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const ir::Insn* in = cu->dom.peek(order[pos]);
            if (in != nullptr && in->srcPc == site->anchorSrcPc && in->op == site->anchorOp) {
                anchorPos = pos;
                break;
            }
        }
        if (anchorPos == static_cast<ca::usize>(-1)) {
            ++failed;
            continue;
        }

        // 全量复核：形状/值重钉，与站点一致才动。
        ca::u8    shapeIdx = 255;
        ca::usize spanLast = 0;
        SiteValue value;
        if (!tryMatchAt(s, cu->dom, order, anchorPos, shapeIdx, spanLast, value)
            || shapeIdx != site->shapeIdx
            || (shapeIdx == 255
                && (value.kind != site->value.kind || value.bits != site->value.bits))) {
            ++failed;
            continue;
        }

        auto slotR = cu->dom.get(order[anchorPos]);
        if (slotR.is_err()) {
            ++failed;
            continue;
        }
        ir::Insn*     anchor   = slotR.unwrap();
        const ir::Insn original = *anchor;

        if (shapeIdx == 255) {
            ir::Insn fresh{};
            if (!rewrite_tools::makeConstLoad(s, simOf(value), fresh)) {
                ++failed;
                continue;
            }
            fresh.srcPc = original.srcPc;
            *anchor     = fresh;
        } else {
            // 替换形状：锚点原地改单条目标指令。
            ir::Insn fresh{};
            fresh.op    = shapeRules()[shapeIdx].target;
            fresh.srcPc = original.srcPc;
            *anchor     = fresh;
        }

        std::vector<ir::InsnId> victims;
        for (ca::usize p = anchorPos + 1; p <= spanLast && p < order.size(); ++p) {
            const ir::Insn* victim = cu->dom.peek(order[p]);
            if (victim == nullptr || victim->op == Op::NOP) continue;
            victims.push_back(order[p]);
        }
        if (!victims.empty() && cu->dom.eraseBatch(victims).is_err()) {
            *anchor = original;
            ++failed;
            continue;
        }
        ++hit;
    }
    return hit;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budgetTemplate)
{
    ScanResult result;
    ca::usize  named    = 0;
    ca::u32    classIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = classIdx++;
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        iter_budget::ClassBudget budget = budgetTemplate;
        ir::ClassSession&        s      = *session;
        const ir::ClassUnit&     unit   = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!scanMethod(s, mi, idx, result, budget)) {
                ++result.classesSkippedByBudget;
                iter_budget::reportExhausted("grunt.ArithFold", "scan",
                                             std::string(entry.uri), budget, named);
                return;
            }
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budgetTemplate)
{
    DirtySet dirty;

    rewrite_tools::FrameContext frames(container);

    std::unordered_map<ca::u32, std::vector<const Site*>> byIdx;
    for (const Site& site : result.sites) byIdx[site.classIdx].push_back(&site);

    ca::usize named  = 0;
    ca::u32   curIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = curIdx++;
        const auto    it  = byIdx.find(idx);
        if (it == byIdx.end()) return;
        const std::vector<const Site*> sitesOf = it->second;
        byIdx.erase(it);

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        iter_budget::ClassBudget budget = budgetTemplate;

        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit             = 0;
        ca::usize failed          = 0;
        ca::usize skippedByBudget = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            dirty.attempted += sitesOfMethod.size();
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    methodHit = rewriteMethod(s, &cu, sitesOfMethod, failed,
                                              skippedByBudget, budget);
                    return methodHit > 0;
                });
            if (!ok) {
                failed += methodHit;
                continue;
            }
            hit += methodHit;
        }
        dirty.failed += failed;
        dirty.skippedByBudget += skippedByBudget;
        if (skippedByBudget > 0) {
            iter_budget::reportExhausted("grunt.ArithFold", "rewrite",
                                         std::string(entry.uri), budget, named);
        }
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        mjbase::model::IResource* raw = container.getResource(entry.uri);
        if (!ca::core::tag_cast::isa<mjbase::model::ClassResource>(raw)) return;
        auto* clsRes = ca::core::tag_cast::cast<mjbase::model::ClassResource>(raw);
        if (clsRes == nullptr) return;
        clsRes->set(std::move(encoded).unwrap());
        dirty.uris.emplace_back(entry.uri);
        dirty.sites += hit;
    });
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

}   // namespace grunt::arith_fold
