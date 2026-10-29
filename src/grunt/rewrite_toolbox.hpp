/// @file        rewrite_toolbox.hpp
/// @brief       REWRITE 工具箱：跨残留共享的改写原语。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口的是「按求值结果
/// 造一条常量装载」——constant_call 与 const_method 的改写段共用同一份，
/// 不许在残留文件里再出现第二份实现。
#pragma once

#include <cstring>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/evaluator/value.hpp"
#include "mjbase/asm/insn/insn_factory.hpp"

namespace grunt::rewrite_tools {

template <class To, class From>
To bitCast(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bitCast size mismatch");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

/// @brief 按求值结果造一条常量装载：短形态优先，放不下才落池。
///
/// 池索引走 `CodeEditor::loadConst` 选形态——`LDC` 的操作数域只有 u1，大类
/// 的池索引轻易超过 255，编码器对超界 `LDC` 直接 Unencodable（insn_codec
/// 的域宽校验）。`loadConst` 负责按实际索引在 `LDC`/`LDC_W` 之间选对。
inline bool makeConstLoad(jbc::ir::ClassSession& session, const mjbase::asm_evaluator::Value& v,
                          jbc::ir::Insn& out)
{
    switch (v.kind) {
    case mjbase::asm_evaluator::Value::Kind::I32: {
        auto r = mjbase::asm_insn::intConstInsn(v.i32);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.internInteger(v.i32);
        if (cp == 0) return false;
        out = jbc::ir::CodeEditor::loadConst(cp);
        return true;
    }
    case mjbase::asm_evaluator::Value::Kind::I64: {
        auto r = mjbase::asm_insn::longConstInsn(v.i64);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addLong(v.i64);
        if (cp == 0 || cp > 0xFFFF) return false;   // LDC2_W 是 CP2 域（u2）
        out.op = jbc::core::Op::LDC2_W;
        out.cp = cp;
        return true;
    }
    case mjbase::asm_evaluator::Value::Kind::F32: {
        auto r = mjbase::asm_insn::floatConstInsn(v.f32);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addFloatBits(bitCast<ca::u32>(v.f32));
        if (cp == 0) return false;
        out = jbc::ir::CodeEditor::loadConst(cp);
        return true;
    }
    case mjbase::asm_evaluator::Value::Kind::F64: {
        auto r = mjbase::asm_insn::doubleConstInsn(v.f64);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addDoubleBits(bitCast<ca::u64>(v.f64));
        if (cp == 0 || cp > 0xFFFF) return false;
        out.op = jbc::core::Op::LDC2_W;
        out.cp = cp;
    }
    default: return false;
    }
}

}   // namespace grunt::rewrite_tools
