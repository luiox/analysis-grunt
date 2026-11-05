/// @file        scan_toolbox.hpp
/// @brief       SCAN 工具箱：跨残留共享的扫描判定。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口两件：
/// `classInitNeutral`——owner 类 `<clinit>` 的「中性」判定，被
/// constant_call（求值前的类初始化门）与 proxy_inline（owner 中性门）共用；
/// `bsmEntries`——类级 BootstrapMethods 条目表读取，被 indy_fold /
/// indy_decode 共用。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

namespace grunt::scan_tools {

/// @brief 取类级 BootstrapMethods 的条目表（没有属性就是空表）。
///
/// 解码前先过注解族深度预检（mjbase #158）：BootstrapMethods 本身不在注解族，
/// 守卫对它恒放行；挂同一道门是让一切 decodeAttr 直调位保持单一形状——将来
/// 若有注解族解码落进来，守卫已在位。
inline const std::vector<jbc::core::BootstrapMethodsAttr::Entry>* bsmEntries(
    jbc::ir::ClassSession& s)
{
    jbc::core::ClassFile& cf = s.backing();
    jbc::core::AttrSlot*  slot = cf.findClassAttr("BootstrapMethods");
    if (slot == nullptr) return nullptr;
    if (!mjbase::util::attrSafeToDecode(cf, *slot)) return nullptr;
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) return nullptr;
    // 不用 dynamic_cast：本仓关 RTTI（/GR-），走 libca 的 tag_cast 约定
    // （AttrBody::kind() 是它的类型标签）。
    auto* attr = ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(decoded.unwrap());
    return attr == nullptr ? nullptr : &attr->entries;
}

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
