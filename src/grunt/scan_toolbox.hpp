/// @file        scan_toolbox.hpp
/// @brief       SCAN 工具箱：跨残留共享的扫描判定。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口的是
/// `classInitNeutral`——owner 类 `<clinit>` 的「中性」判定，被
/// constant_call（求值前的类初始化门）与 proxy_inline（owner 中性门）共用。
#pragma once

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

namespace grunt::scan_tools {

/// @brief 判 owner 类 `<clinit>` 是否「中性」（缺失，或只剩 NOP/RETURN 且无
///        异常表）。
///
/// 这是保守面而不是判据：折叠掉指向 owner 的某次主动使用（invokestatic 调
/// 用、字段访问内联）会改变类初始化时机，只有确认初始化器什么都不做时才
/// 允许那次使用消失。`<clinit>` 带任何实体指令或异常表都判非中性。
inline bool classInitNeutral(jbc::ir::ClassSession& session)
{
    namespace ir = jbc::ir;
    using jbc::core::Op;

    constexpr ca::usize kNoIndex = static_cast<ca::usize>(-1);

    const ir::ClassUnit& unit        = session.unit();
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

}  // namespace grunt::scan_tools
