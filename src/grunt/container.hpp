/// @file        container.hpp
/// @brief       家族无关的画布：容器遍历、类会话开启、类字节回写。
///
/// 这里只做「拿到字节 / 交出字节」这一层，不含任何 Grunt 知识，也不含任何
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

#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

namespace grunt {

/// @brief 容器内一个类条目的视图（uri 与字节都指向容器，不持有）。
struct ClassEntry
{
    std::string_view             uri;
    const std::vector<ca::u8>*   bytes = nullptr;
};

/// @brief 按资源序回调容器内全部非空类条目；非类资源与空条目直接跳过。
template <class Fn>
void forEachClass(const mjbase::model::ResourceContainer& container, Fn&& fn)
{
    container.visitResources([&](std::string_view uri, const mjbase::model::IResource& resource) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(&resource);
        if (cls == nullptr || cls->get().empty()) return;
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

/// @brief 回写类字节（替换容器内同 uri 条目）。
inline void replaceClass(mjbase::model::ResourceContainer& container, std::string_view uri,
                         std::vector<ca::u8> bytes)
{
    std::string owned(uri);
    container.replaceResource(
        owned, std::make_unique<mjbase::model::ClassResource>(owned, std::move(bytes)));
}

}   // namespace grunt
