/// @file        container.hpp
/// @brief       家族无关的画布：容器遍历、类会话开启、类字节回写。
///
/// 这里只做拿到字节 / 交出字节这一层，不含任何 Grunt 知识，也不含任何
/// 残留判据。三个工具箱与各残留都从本文件取画布，避免各自写一遍
/// `getResources` + `tag_cast` + `makeSessionFromBytes` 的三连。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"

#include "grunt/pass_clock.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/util/annotation_depth.hpp"

namespace grunt {

/// @brief 容器内一个类条目的视图（uri 与字节都指向容器，不持有）。
struct ClassEntry
{
    std::string_view             uri;
    const std::vector<ca::u8>*   bytes = nullptr;
};

/// @brief 按资源序回调容器内全部非空类条目；非类资源与空条目直接跳过。
///
/// pass 级时间片检查（pass_clock，见该头注）：有活动片且已超墙时，余下类
/// 不再下发（整类保守跳过），首停类 stderr 点名 + 逐类计数。
template <class Fn>
void forEachClass(const mjbase::model::ResourceContainer& container, Fn&& fn)
{
    container.visitResources([&](std::string_view uri, const mjbase::model::IResource& resource) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(&resource);
        if (cls == nullptr || cls->get().empty()) return;
        if (pass_clock::gActiveClock != nullptr &&
            !pass_clock::gActiveClock->admit(uri)) {
            return;
        }
        fn(ClassEntry{uri, &cls->get()});
    });
}

/// @brief 只读会话：解析失败返回空指针（调用方按 fail-closed 跳过该类）。
inline std::shared_ptr<jbc::ir::ClassSession> openSession(const std::vector<ca::u8>& bytes)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) return nullptr;
    return std::move(loaded).unwrap();
}

/// @brief 类级注解族深度预检：CP 压缩（cp_compact 面）的守卫前置。
///
/// jbc::ir::cp_compact 会对全部属性槽（类/字段/方法 + Code 嵌套）decodeAttr
/// 以重映射注解内的 CP 引用，而 libjbc attr_codec 注解族解码互递归无界
/// （mjbase #158 同源，上游遗留）：敌意深嵌注解不走 Result 错误通道，直接
/// 栈溢出。故 compact 前对同一批槽位过一遍 attrSafeToDecode，任一注解族槽
/// 越限即返回 false，调用方按既有 compact 失败通道原样保真跳过该类
/// （CP 不重编号，字节不动）。Code 槽本身解码有界（非注解族），仅借它下探
/// 嵌套属性；Code 解不开时放行，cp_compact 对它回 MalformedAttr，优雅。
inline bool classAnnoDepthSafe(jbc::core::ClassFile& cf)
{
    const auto slotsOk = [&](const std::vector<jbc::core::AttrSlot>& slots) {
        for (const jbc::core::AttrSlot& slot : slots) {
            if (!mjbase::util::attrSafeToDecode(cf, slot)) return false;
        }
        return true;
    };
    if (!slotsOk(cf.attrs)) return false;
    for (const jbc::core::MemberInfo& f : cf.fields) {
        if (!slotsOk(f.attrs)) return false;
    }
    for (jbc::core::MemberInfo& m : cf.methods) {
        if (!slotsOk(m.attrs)) return false;
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) continue;   // cp_compact 自会按 MalformedAttr 优雅失败
            auto& code = static_cast<jbc::core::CodeAttr&>(*d.unwrap());
            if (!slotsOk(code.attrs)) return false;
        }
    }
    return true;
}

/// @brief 回写类字节（替换容器内同 uri 条目）。
inline void replaceClass(mjbase::model::ResourceContainer& container, std::string_view uri,
                         std::vector<ca::u8> bytes)
{
    std::string owned(uri);
    container.replaceResource(
        owned, std::make_unique<mjbase::model::ClassResource>(owned, std::move(bytes)));
}

/// @brief 按 uri 取类字节；缺失或非类资源返回空。
inline const std::vector<ca::u8>* findClassBytes(
    const mjbase::model::ResourceContainer& container, std::string_view uri)
{
    const mjbase::model::IResource* raw = container.getResource(uri);
    const auto* cls =
        ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(raw);
    return cls == nullptr ? nullptr : &cls->get();
}

/// @brief 容器内类条目数（proof 的类数检查用）。
///
/// 计数挂起时间片：类数不增检查必须看到全量类，不许被超墙截短。
inline ca::usize countClasses(const mjbase::model::ResourceContainer& container)
{
    ca::usize           n = 0;
    pass_clock::Suspend suspend;
    forEachClass(container, [&](const ClassEntry&) { ++n; });
    return n;
}

}   // namespace grunt
