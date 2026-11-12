/// @file        flags_strip.cpp
/// @brief       残留 `flags_strip` 的三段实现：SCAN / REWRITE / PROOF。
///
/// 判据等级：S2 结构事实（编译器不可能生成的标志组合）。Scan 纯读：逐类
/// 开只读会话直接看 backing 的 `ClassFile`（不 promote，方法体与本残留
/// 无关），成员判据见 `memberFlagsClearable`；站点用 backing 索引定位，
/// 掩码在 scan 定死。保守面（不收站点）：实例方法的 `ACC_BRIDGE`（可能是
/// 真实泛型桥接器）、实例成员的 `ACC_SYNTHETIC`（`this$0` 等）、类级
/// `ACC_SYNTHETIC`。
///
/// Rewrite 纯写：按 uri 分组，逐类开会话 → **按 scan 给的掩码清位**（不
/// 回看判据，rewrite 只消费 ScanResult）→ `writeClass` → 回写容器。无
/// promote/lower：不改方法体，没有重定位面。索引越界的站点计数
/// （`sitesOutOfRange`）不猜。
///
/// Proof：类数不增 + 脏类逐个可重新解析（与 constant_call 同一口径）。标志
/// 清理不改字节码结构，无帧重算面，逐类加载校验由语料回归覆盖。
#include "grunt/flags_strip.hpp"

#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"

#include "grunt/container.hpp"

namespace grunt::flags_strip {

namespace {

constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccBridge    = 0x0040;
constexpr ca::u16 kAccEnum      = 0x4000;
constexpr ca::u16 kAccSynthetic = 0x1000;

/// @brief 一份类字节上的成员标志是否可清（判据本体，scan 专用）。
///
/// 检查：非枚举类（`$VALUES` 等枚举机制字段是真实的 static synthetic）+ 成员
/// static + 名字不以 `$` 开头，各工具链的编译器合成物一律带 `$`
/// （`access$000`、`$SWITCH_TABLE$`、`$assertionsDisabled`），混淆器注入的
/// 伪标志成员是改名后的普通名字，两者按名字形状可分。方法清 BRIDGE|
/// SYNTHETIC，字段清 SYNTHETIC。
ca::u16 bogusMask(const jbc::core::ClassFile& cf, bool isMethod, ca::u16 access,
                  std::string_view name) noexcept
{
    if ((cf.access & kAccEnum) != 0) return 0;
    if ((access & kAccStatic) == 0) return 0;
    if (!name.empty() && name.front() == '$') return 0;
    if (isMethod) return static_cast<ca::u16>(access & (kAccBridge | kAccSynthetic));
    return static_cast<ca::u16>(access & kAccSynthetic);
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        const jbc::core::ClassFile& cf = session->backing();

        for (ca::usize i = 0; i < cf.methods.size(); ++i) {
            const ca::u16 mask = bogusMask(cf, true, cf.methods[i].access,
                                           cf.utf8(cf.methods[i].nameCp));
            if (mask == 0) continue;
            result.sites.push_back(
                FlagSite{std::string(entry.uri), true, i, mask});
        }
        for (ca::usize i = 0; i < cf.fields.size(); ++i) {
            const ca::u16 mask = bogusMask(cf, false, cf.fields[i].access,
                                           cf.utf8(cf.fields[i].nameCp));
            if (mask == 0) continue;
            result.sites.push_back(
                FlagSite{std::string(entry.uri), false, i, mask});
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    std::unordered_map<std::string, std::vector<const FlagSite*>> byUri;
    for (const FlagSite& site : result.sites) byUri[site.uri].push_back(&site);

    // 两遍式：上面只收动作清单，这里逐类开会话 → 清位 → 编码 → 立即回写。
    // pending 全量驻留会在整 jar 级站点（数千类 × 类字节）把内存打穿；逐类
    // 写回的峰值是单类会话。
    for (auto& [uri, sites] : byUri) {
        const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
        if (bytes == nullptr) continue;
        auto session = openSession(*bytes);
        if (!session) continue;
        jbc::core::ClassFile& cf = session->backing();

        for (const FlagSite* site : sites) {
            jbc::core::MemberInfo* member = nullptr;
            if (site->isMethod) {
                if (site->index >= cf.methods.size()) {
                    ++dirty.sitesOutOfRange;
                    continue;
                }
                member = &cf.methods[site->index];
            } else {
                if (site->index >= cf.fields.size()) {
                    ++dirty.sitesOutOfRange;
                    continue;
                }
                member = &cf.fields[site->index];
            }
            // 掩码在 scan 定死：这里只清位，不回看判据。
            member->access = static_cast<ca::u16>(member->access & ~site->clearMask);
            ++dirty.membersCleared;
        }

        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) continue;
        replaceClass(container, uri, std::move(encoded).unwrap());
        dirty.uris.push_back(uri);
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

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

}   // namespace grunt::flags_strip
