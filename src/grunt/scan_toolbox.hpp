/// @file        scan_toolbox.hpp
/// @brief       SCAN 工具箱：跨残留共享的扫描判定、码元面编解码与方法索引。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口五件：
/// `classInitNeutral`——owner 类 `<clinit>` 的「中性」判定，被
/// constant_call（求值前的类初始化门）与 proxy_inline（owner 中性门）共用；
/// `bsmEntries`——类级 BootstrapMethods 条目表读取，被 indy_fold /
/// indy_decode 共用；`mutf8ToUnits`/`decryptXor`——MUTF-8 码元面解码与逐码
/// 元自逆 XOR，被 indy_restore / indy_decode / material_key_restore 共用
/// （原三 pass 各持同形私有副本，B2 单源化）；`MethodIndex`/
/// `buildMethodIndex`——容器方法索引（keyOf 复合键），constant_call 与
/// gate_processor_fold 同源共用（原两 pass 各持副本，B2 单源化），
/// `keyOf` 另被 proxy_inline / field_access_proxy / const_method 的定义表
/// 共用（原五 pass 各持同形副本）。
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"

#include "grunt/container.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

namespace grunt::scan_tools {

/// @brief MUTF-8 → UTF-16 码元（不经严格 UTF-8 解码：密文里合法地存在
///        Java 孤代理码元——classfile 以 CESU-8 形 3 字节承载，严格 UTF-8
///        解码器会拒收，而加密语义只在码元层面异或）。MUTF-8 的 NUL 特例
///        （0xC0 0x80）一并处理；4 字节形 / 挂续字节（classfile Utf8 不出现）
///        返回 false。
inline bool mutf8ToUnits(std::string_view in, std::vector<ca::u16>& out)
{
    out.clear();
    ca::usize i = 0;
    while (i < in.size()) {
        const ca::u8 b0 = static_cast<ca::u8>(in[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<ca::u16>(b0));
            ++i;
            continue;
        }
        if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= in.size()) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            if ((b1 & 0xC0) != 0x80) return false;
            out.push_back(static_cast<ca::u16>(((b0 & 0x1F) << 6) | (b1 & 0x3F)));
            i += 2;
            continue;
        }
        if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= in.size()) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            const ca::u8 b2 = static_cast<ca::u8>(in[i + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            out.push_back(static_cast<ca::u16>(((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6)
                                               | (b2 & 0x3F)));
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

/// @brief 逐码元自逆 XOR（每 UTF-16 码元 XOR key 低 16 位，Java `char ^ int`
///        后 `i2c` 的语义），产物落 UTF-8——异常码元按 3 字节原样承载，垃圾
///        解密结果会死在下游 desc 互证，不在这里猜。@return false = MUTF-8
///        解码失败。
inline bool decryptXor(std::string_view encrypted, ca::i32 key, std::string& out)
{
    std::vector<ca::u16> units;
    if (!mutf8ToUnits(encrypted, units)) return false;
    const ca::u16 lo = static_cast<ca::u16>(key);
    out.clear();
    for (ca::u16& u : units) {
        u = static_cast<ca::u16>(u ^ lo);
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return true;
}

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

/// @brief `owner\nname\ndesc` 复合键（方法索引与各定义表共用）。
///
/// 原 constant_call / gate_processor_fold / proxy_inline / field_access_proxy
/// / const_method 五份同形私有副本单源化至此（B2）。
inline std::string keyOf(std::string_view owner, std::string_view name,
                         std::string_view desc)
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

/// @brief 容器方法定位值（constant_call / gate_processor_fold 同源）。
///
/// 原两份副本只在 constant_call 多一个无人读的 `desc` 字段，统一形落在
/// 三字段（B2 单源化，逐处登记）。
struct MethodLoc
{
    std::string owner;
    ca::usize   index  = 0;
    ca::u32     access = 0;
};

/// @brief 容器方法索引：`keyOf` 键 → 声明位置。
using MethodIndex = std::unordered_map<std::string, MethodLoc>;

/// @brief 重名类（两个容器条目 `thisName` 撞同一 `keyOf` 键）的取舍口径。
///
/// 两 pass 原生有别且不可猜统一：constant_call 是 `emplace`（先到先得），
/// gate_processor_fold 是 `insert_or_assign`（后到后得）——B2 单源化时以
/// 本枚举逐处钉住原口径，行为逐位不动。
enum class IndexDup : ca::u8 { KeepFirst, KeepLast };

/// @brief 容器方法索引构建（constant_call / gate_processor_fold 的
///        `buildIndex` 同源收口）：每类开只读会话，全方法入索引。
///
/// guard 是调用方带入的 per-class fail-closed 包裹（本仓 scanGuard 面，
/// stderr 前缀随 pass 各异，不在此固化）：guard 返回 false = 该类跳过、
/// 余类继续。遍历走 container.hpp 的 `forEachClass`，pass 级时间片门语义
/// 与原两份副本逐位同。
template <class Guard>
inline void buildMethodIndex(const mjbase::model::ResourceContainer& container,
                             MethodIndex& index, IndexDup dup, Guard&& guard)
{
    namespace ir = jbc::ir;

    forEachClass(container, [&](const ClassEntry& entry) {
        guard("index", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const ir::ClassUnit&   unit = session->unit();
            const std::string_view self = session->backing().thisName();
            for (ca::usize i = 0; i < unit.methods.size(); ++i) {
                const ir::MethodUnit&  mu   = unit.methods[i];
                const std::string_view name = unit.symbols().str(mu.name);
                const std::string_view desc = unit.symbols().str(mu.desc);
                if (dup == IndexDup::KeepFirst) {
                    index.emplace(keyOf(self, name, desc),
                                  MethodLoc{std::string(self), i, mu.access});
                } else {
                    index.insert_or_assign(keyOf(self, name, desc),
                                           MethodLoc{std::string(self), i, mu.access});
                }
            }
        });
    });
}

}  // namespace grunt::scan_tools
