/// @file        jsim_bridge.hpp
/// @brief       CodeUnit → jsim::Method 的边界胶水（设计稿 §9：桥由消费方自备）。
///
/// 转换只做「形状搬运」，不做语义解释：指令序列摊成 pc 索引、标签解析成 pc、
/// CP 操作数解析成 jsim 侧的紧凑表。任何一步拿不到确定信息就返回 false 并给出
/// 原因——宁可整方法放弃，也不产出一个「看起来能跑」的半截方法。
///
/// 两个形状差异要专门处理：
///
/// 1. libjbc 的跳转目标是 LabelId（句柄域），jsim 的跳转目标是 pc（序号域）。
///    这里先把 `order()` 的位置当下标，再用 resolveLabel 把标签折成下标；
///    锚在方法末尾的标签折成 `code.size()`。
/// 2. libjbc 的 CP 操作数是 backing 常量池索引（可能上万），jsim 的 `Insn::a`
///    在 ldc 族里当紧凑表下标。这里只对 ldc 族做索引重映射——其余指令的 `a`
///    是槽号/表下标，语义不同，绝不混用。
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "jsim/engine.hpp"

namespace grunt::jsim_bridge {

/// @brief 描述符首字符 → 值类型（字段类型、返回类型共用）。
inline bool valTypeOfDescChar(char c, jsim::ValType& out) noexcept
{
    switch (c) {
    case 'I':
    case 'Z':
    case 'B':
    case 'C':
    case 'S': out = jsim::ValType::I32; return true;
    case 'J': out = jsim::ValType::I64; return true;
    case 'F': out = jsim::ValType::F32; return true;
    case 'D': out = jsim::ValType::F64; return true;
    case 'L':
    case '[': out = jsim::ValType::Ref; return true;
    default: return false;
    }
}

/// @brief 方法描述符的返回类型。`V` 返回 false（void，无返回值）。
inline bool retTypeOfDesc(std::string_view desc, jsim::ValType& out) noexcept
{
    const std::size_t close = desc.find(')');
    if (close == std::string_view::npos || close + 1 >= desc.size()) return false;
    if (desc[close + 1] == 'V') return false;
    return valTypeOfDescChar(desc[close + 1], out);
}

/// @brief 字段描述符 → 值类型。
inline bool fieldTypeOfDesc(std::string_view desc, jsim::ValType& out) noexcept
{
    if (desc.empty()) return false;
    return valTypeOfDescChar(desc.front(), out);
}

/// @brief 方法描述符的参数列表 → 值类型序列。`(`/`)` 缺失返回 false。
///
/// 引用类型（含数组）一律折成 Ref：跨方法编排只需要知道「占一个栈位、是个引用」，
/// 具体类名不参与弹栈。cat2 的宽度由 ValType 自身携带，调用方据此算槽位。
inline bool paramTypesOfDesc(std::string_view desc, std::vector<jsim::ValType>& out) noexcept
{
    out.clear();
    const std::size_t open = desc.find('(');
    if (open == std::string_view::npos) return false;
    std::size_t i = open + 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        if (c == '[') {
            while (i < desc.size() && desc[i] == '[') ++i;
            if (i < desc.size() && desc[i] == 'L') {
                while (i < desc.size() && desc[i] != ';') ++i;
            }
            ++i;
            out.push_back(jsim::ValType::Ref);
            continue;
        }
        if (c == 'L') {
            while (i < desc.size() && desc[i] != ';') ++i;
            ++i;
            out.push_back(jsim::ValType::Ref);
            continue;
        }
        jsim::ValType t = jsim::ValType::I32;
        if (!valTypeOfDescChar(c, t)) return false;
        out.push_back(t);
        ++i;
    }
    return i < desc.size();
}

namespace detail {

/// @brief Class 常量 → 内部名。
inline std::string classNameOf(const jbc::core::ConstantPool& cp, ca::u32 classCp)
{
    if (!cp.valid(classCp)) return {};
    const jbc::core::ConstantPool::Entry& e = cp.at(classCp);
    if (e.tag != jbc::core::CpTag::Class) return {};
    return std::string(cp.utf8Of(e.a));
}

/// @brief 成员引用（Fieldref/Methodref/InterfaceMethodref）→ owner/name/desc。
inline bool memberRefOf(const jbc::core::ConstantPool& cp, ca::u32 refCp,
                        std::string& owner, std::string& name, std::string& desc)
{
    if (!cp.valid(refCp)) return false;
    const jbc::core::ConstantPool::Entry& e = cp.at(refCp);
    if (!cp.valid(e.a) || !cp.valid(e.b)) return false;
    const jbc::core::ConstantPool::Entry& nt = cp.at(e.b);
    if (nt.tag != jbc::core::CpTag::NameAndType) return false;
    owner = classNameOf(cp, e.a);
    name  = std::string(cp.utf8Of(nt.a));
    desc  = std::string(cp.utf8Of(nt.b));
    return !owner.empty() && !name.empty();
}

/// @brief 标签句柄 → 稳定的散列键（owner 在单个 DOM 内恒定，故只取 slot/gen）。
inline ca::u64 labelKey(jbc::ir::LabelId id) noexcept
{
    return (static_cast<ca::u64>(id.slot) << 32) | static_cast<ca::u64>(id.gen);
}

inline bool isLdcFamily(jbc::core::Op op) noexcept
{
    return op == jbc::core::Op::LDC || op == jbc::core::Op::LDC_W
           || op == jbc::core::Op::LDC2_W;
}

}   // namespace detail

/// @brief 把 ldc 的 CP 条目搬进 jsim 的紧凑表。
///
/// MUTF-8 → UTF-8 不做转换：Grunt 的密文与明文都在 BMP 内，CESU-8 代理对
/// 只出现在增补平面字符上；真遇到时按字节原样交给上层，由扩展包决定怎么读。
inline bool appendCpEntry(const jbc::core::ConstantPool& cp, ca::u32 cpIndex,
                          jsim::CpEntry& out)
{
    if (!cp.valid(cpIndex)) return false;
    const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
    switch (e.tag) {
    case jbc::core::CpTag::Integer:
        out.kind      = jsim::CpEntry::Kind::Int;
        out.intValue  = e.ival;
        return true;
    case jbc::core::CpTag::Float:
        out.kind     = jsim::CpEntry::Kind::Float;
        out.intValue = static_cast<ca::i32>(e.ival);   // 位型原样
        return true;
    case jbc::core::CpTag::Long:
        out.kind      = jsim::CpEntry::Kind::Long;
        out.longValue = e.lval;
        return true;
    case jbc::core::CpTag::Double:
        out.kind      = jsim::CpEntry::Kind::Double;
        out.longValue = e.lval;                        // 位型原样
        return true;
    case jbc::core::CpTag::String: {
        if (!cp.valid(e.a)) return false;
        out.kind     = jsim::CpEntry::Kind::Str;
        out.strValue = std::string(cp.utf8Of(e.a));
        return true;
    }
    case jbc::core::CpTag::Class:
        out.kind      = jsim::CpEntry::Kind::Class;
        out.className = detail::classNameOf(cp, cpIndex);
        return !out.className.empty();
    default: return false;
    }
}

/// @brief 单方法转换。失败时 `why` 带原因（不猜、不半截）。
inline bool toSimMethod(jbc::ir::ClassSession& s, ca::usize methodIndex, jsim::Method& out,
                        std::string* why = nullptr)
{
    auto fail = [why](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };

    if (!jbc::ir::hasCode(s, methodIndex)) return fail("方法无 Code 属性");

    auto promoted = jbc::ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return fail("promoteMethod 失败");
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return fail("promote 产物为空");

    const jbc::ir::ClassUnit& unit = s.unit();
    if (methodIndex >= unit.methods.size()) return fail("方法下标越界");
    const jbc::ir::MethodUnit& mu = unit.methods[methodIndex];

    const std::vector<jbc::ir::InsnId>& order = cu->dom.order();
    if (order.empty()) return fail("方法体为空");

    // 指令句柄 → pc 下标
    std::unordered_map<ca::u64, ca::usize> insnPc;
    insnPc.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) {
        insnPc[(static_cast<ca::u64>(order[i].slot) << 32) | order[i].gen] = i;
    }

    // 标签 → pc：resolveLabel 给出锚定指令，末尾标签折成 code.size()。
    std::unordered_map<ca::u64, ca::usize> labelPc;
    auto resolveLabelPc = [&](jbc::ir::LabelId label, ca::usize& pc) -> bool {
        const ca::u64 key = detail::labelKey(label);
        auto it = labelPc.find(key);
        if (it != labelPc.end()) {
            pc = it->second;
            return true;
        }
        auto resolved = cu->dom.resolveLabel(label);
        if (resolved.is_err()) return false;
        const jbc::ir::LabelTarget target = resolved.unwrap();
        if (target.isEnd()) {
            pc = order.size();
        } else {
            auto at = insnPc.find((static_cast<ca::u64>(target.insn.slot) << 32) | target.insn.gen);
            if (at == insnPc.end()) return false;
            pc = at->second;
        }
        labelPc.emplace(key, pc);
        return true;
    };

    const jbc::core::ConstantPool& cp = s.backing().cp;

    out          = jsim::Method{};
    out.name     = std::string(unit.symbols().str(mu.name));
    out.desc     = std::string(unit.symbols().str(mu.desc));
    out.maxLocals = cu->maxLocals;
    out.code.reserve(order.size());

    // ldc 族的 CP 索引重映射表
    std::unordered_map<ca::u32, ca::i32> cpToLocal;

    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = cu->dom.peek(order[i]);
        if (in == nullptr) return fail("指令句柄悬垂");

        jsim::Insn ji;
        ji.opcode = static_cast<ca::u8>(in->op);

        // ---- ldc 族：CP 操作数 → 紧凑表下标 ----
        if (detail::isLdcFamily(in->op)) {
            auto mapped = cpToLocal.find(in->cp);
            if (mapped == cpToLocal.end()) {
                jsim::CpEntry entry;
                if (!appendCpEntry(cp, in->cp, entry)) return fail("ldc 常量类型不支持");
                const ca::i32 local = static_cast<ca::i32>(out.cp.size());
                out.cp.push_back(std::move(entry));
                mapped = cpToLocal.emplace(in->cp, local).first;
            }
            ji.a = mapped->second;
            out.code.push_back(ji);
            continue;
        }

        switch (in->op) {
        // ---- 调用：owner/name/desc + 返回类型 ----
        case jbc::core::Op::INVOKEVIRTUAL:
        case jbc::core::Op::INVOKESPECIAL:
        case jbc::core::Op::INVOKESTATIC:
        case jbc::core::Op::INVOKEINTERFACE: {
            jsim::CallRef call;
            if (!detail::memberRefOf(cp, in->cp, call.owner, call.name, call.desc)) {
                return fail("调用点引用解析失败");
            }
            jsim::ValType rt = jsim::ValType::I32;
            call.hasReturn    = retTypeOfDesc(call.desc, rt);
            call.retType      = call.hasReturn ? rt : jsim::ValType::I32;
            call.isStatic     = in->op == jbc::core::Op::INVOKESTATIC;
            ji.a              = static_cast<ca::i32>(out.calls.size());
            out.calls.push_back(std::move(call));
            break;
        }
        // ---- invokedynamic：v1 不建模（设计 §11），留一个可读的接管键 ----
        case jbc::core::Op::INVOKEDYNAMIC: {
            jsim::CallRef call;
            call.owner = "<invokedynamic>";
            call.name  = std::to_string(in->cp);
            call.desc  = "";
            call.hasReturn = true;
            ji.a       = static_cast<ca::i32>(out.calls.size());
            out.calls.push_back(std::move(call));
            break;
        }
        // ---- 字段：owner/name/desc + 字段类型 ----
        case jbc::core::Op::GETSTATIC:
        case jbc::core::Op::PUTSTATIC:
        case jbc::core::Op::GETFIELD:
        case jbc::core::Op::PUTFIELD: {
            jsim::FieldRef field;
            if (!detail::memberRefOf(cp, in->cp, field.owner, field.name, field.desc)) {
                return fail("字段引用解析失败");
            }
            if (!fieldTypeOfDesc(field.desc, field.type)) return fail("字段类型不支持");
            ji.a = static_cast<ca::i32>(out.fields.size());
            out.fields.push_back(std::move(field));
            break;
        }
        // ---- 类操作数：类名进 allocTypes（jsim 的 New/anewarray 走这张表）----
        case jbc::core::Op::NEW:
        case jbc::core::Op::ANEWARRAY:
        case jbc::core::Op::MULTIANEWARRAY:
        case jbc::core::Op::CHECKCAST:
        case jbc::core::Op::INSTANCEOF: {
            std::string cls = detail::classNameOf(cp, in->cp);
            if (cls.empty()) return fail("类常量解析失败");
            ji.a = static_cast<ca::i32>(out.allocTypes.size());
            out.allocTypes.push_back(std::move(cls));
            ji.b = in->b;   // multianewarray 的 dims
            break;
        }
        // ---- 分支：标签 → pc ----
        case jbc::core::Op::GOTO:
        case jbc::core::Op::IFEQ:
        case jbc::core::Op::IFNE:
        case jbc::core::Op::IFLT:
        case jbc::core::Op::IFGE:
        case jbc::core::Op::IFGT:
        case jbc::core::Op::IFLE:
        case jbc::core::Op::IF_ICMPEQ:
        case jbc::core::Op::IF_ICMPNE:
        case jbc::core::Op::IF_ICMPLT:
        case jbc::core::Op::IF_ICMPGE:
        case jbc::core::Op::IF_ICMPGT:
        case jbc::core::Op::IF_ICMPLE:
        case jbc::core::Op::IF_ACMPEQ:
        case jbc::core::Op::IF_ACMPNE:
        case jbc::core::Op::IFNULL:
        case jbc::core::Op::IFNONNULL: {
            ca::usize target = 0;
            if (!resolveLabelPc(in->target, target)) return fail("分支目标解析失败");
            ji.a = static_cast<ca::i32>(target);
            break;
        }
        case jbc::core::Op::TABLESWITCH: {
            jsim::SwitchTable table;
            table.min       = in->a;
            table.max       = in->a + static_cast<ca::i32>(in->targets.size()) - 1;
            table.targets.reserve(in->targets.size());
            for (const jbc::ir::LabelId label : in->targets) {
                ca::usize target = 0;
                if (!resolveLabelPc(label, target)) return fail("tableswitch 目标解析失败");
                table.targets.push_back(static_cast<ca::u32>(target));
            }
            ca::usize defaultPc = 0;
            if (!resolveLabelPc(in->target, defaultPc)) return fail("tableswitch default 解析失败");
            table.defaultPc = static_cast<ca::u32>(defaultPc);
            ji.a            = static_cast<ca::i32>(out.switchTables.size());
            out.switchTables.push_back(std::move(table));
            break;
        }
        case jbc::core::Op::LOOKUPSWITCH: {
            jsim::LookupTable table;
            if (in->keys.size() != in->targets.size()) return fail("lookupswitch 键/目标不齐");
            table.pairs.reserve(in->keys.size());
            for (ca::usize k = 0; k < in->keys.size(); ++k) {
                ca::usize target = 0;
                if (!resolveLabelPc(in->targets[k], target)) return fail("lookupswitch 目标解析失败");
                table.pairs.emplace_back(in->keys[k], static_cast<ca::u32>(target));
            }
            ca::usize defaultPc = 0;
            if (!resolveLabelPc(in->target, defaultPc)) return fail("lookupswitch default 解析失败");
            table.defaultPc = static_cast<ca::u32>(defaultPc);
            ji.a            = static_cast<ca::i32>(out.lookupTables.size());
            out.lookupTables.push_back(std::move(table));
            break;
        }
        // ---- 其余：a/b 原样搬运（槽号、iinc 增量、newarray atype）----
        default: {
            ji.a = in->a;
            ji.b = in->b;
            break;
        }
        }

        out.code.push_back(ji);
    }

    return true;
}

}   // namespace grunt::jsim_bridge
