/// @file        knowledge/era.hpp
/// @brief       Grunt 家族知识：版本（Era）与家族常量。
///
/// CONVENTION §6.3：版本分支只允许出现在 scan 的细找里，且必须是 S3 级判据。
/// 本文件是**唯一**允许出现版本相关字面量的地方（字面量本体在
/// kasumi_names.hpp / booger_names.hpp，本文件只做探测裁决）。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/booger_names.hpp"
#include "grunt/knowledge/kasumi_names.hpp"

namespace grunt::knowledge {

/// @brief 家族版本。
enum class Era : ca::u8 {
    Unknown = 0,  ///< 判据不足：fail-closed，不做猜测性改写
    Grunt2,       ///< Gruntpocalypse 2.x
    Grunt3,       ///< Grunteon
    Kasumi,       ///< Kasumi Obfuscator（indy 全量改写 + @Metadata + 反 LLM 资源）
};

/// @brief 版本探测（每家族仓固定提供这一个函数）。命中即判：三个 Kasumi
///        指纹任一命中即 `Era::Kasumi`（混淆 jar 由单一混淆器版本产出，不做
///        加权投票）；全部落空返回 `Unknown`。
///
/// - **命名空间**：容器内有 `dev/kasumi/obfuscator` 前缀的类（混淆器运行时
///   类必然在场——Main-Class 即在其中，25 类样本全貌见 notes §1）；
/// - **8 参 BSM 指纹**：任一 BootstrapMethods 条目的 MethodHandle 方法描述符
///   等于 `kKasumiBootstrapDesc`（Kasumi 的 indy 形态唯一签名）；
/// - **@Metadata 注解**：类/字段/方法任一挂载点带
///   `@Ldev/kasumi/internal/Metadata;`（每类一个，Kasumi 特有）。
///
/// Grunt2/Grunt3 当前**没有可靠判据**，理由如下：
///
/// - 类/成员名的 `U+200E` 尾缀在两代里都存在（Grunt2 的
///   `ClassRenameTransformer::suffix` 与 Grunt3 的 `FieldRenamer::suffix`
///   同源），不能当版本判据；
/// - 调用点模板、数字混淆形状在两代之间没有观测到稳定差异。
///
/// `constant_call` 不消费 Era：它的判据是语义事实（纯函数 + 常量实参 +
/// 求值成功），与混淆器版本无关。等 Grunt2/Grunt3 真正版本敏感的残留出现时，
/// 再把探测规则补在这里，而不是散进各残留。
inline Era probeEra(const mjbase::model::ResourceContainer& container)
{
    bool namespaceHit = false;
    bool bsmHit       = false;
    bool annoHit      = false;

    forEachClass(container, [&](const ClassEntry& entry) {
        if (namespaceHit && bsmHit && annoHit) return;
        auto session = openSession(*entry.bytes);
        if (!session) return;

        const jbc::ir::ClassUnit& unit = session->unit();
        const std::string_view    self = unit.symbols().str(unit.thisClass);
        if (!namespaceHit && self.rfind(kKasumiNamespace, 0) == 0) namespaceHit = true;

        // ---- 8 参 BSM 指纹：BootstrapMethods 条目 → MethodHandle → desc ----
        if (!bsmHit) {
            jbc::core::ClassFile&     cf    = session->backing();
            jbc::core::AttrSlot*      slot  = cf.findClassAttr("BootstrapMethods");
            const auto checkEntry = [&](const jbc::core::BootstrapMethodsAttr::Entry& bsm) {
                if (!cf.cp.valid(bsm.methodRef)) return;
                const jbc::core::ConstantPool::Entry& mh = cf.cp.at(bsm.methodRef);
                if (mh.tag != jbc::core::CpTag::MethodHandle || !cf.cp.valid(mh.b)) return;
                const jbc::core::ConstantPool::Entry& ref = cf.cp.at(mh.b);
                if (ref.tag != jbc::core::CpTag::Methodref
                    && ref.tag != jbc::core::CpTag::InterfaceMethodref) {
                    return;
                }
                if (!cf.cp.valid(ref.b)) return;
                const jbc::core::ConstantPool::Entry& nat = cf.cp.at(ref.b);
                if (nat.tag != jbc::core::CpTag::NameAndType || !cf.cp.valid(nat.b)) return;
                bsmHit = cf.utf8(nat.b) == kKasumiBootstrapDesc;
            };
            if (slot != nullptr && mjbase::util::attrSafeToDecode(cf, *slot)) {
                auto decoded = cf.decodeAttr(*slot);
                if (decoded.is_ok()) {
                    // 不用 dynamic_cast：本仓关 RTTI，走 libca tag_cast 约定。
                    const auto* attr = ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(
                        decoded.unwrap());
                    if (attr != nullptr) {
                        for (const jbc::core::BootstrapMethodsAttr::Entry& bsm : attr->entries) {
                            checkEntry(bsm);
                            if (bsmHit) break;
                        }
                    }
                }
            }
        }

        // ---- @Metadata 注解：类/字段/方法三张挂载面（对齐 booger 普查口径）----
        if (!annoHit) {
            jbc::core::ClassFile& cf = session->backing();
            const auto hasMetadata = [&](std::vector<jbc::core::AttrSlot>& slots) {
                for (jbc::core::AttrSlot& as : slots) {
                    // 注解族解码互递归无界（mjbase #158），预检拒绝按未命中跳过。
                    if (!mjbase::util::attrSafeToDecode(cf, as)) continue;
                    auto d = cf.decodeAttr(as);
                    if (d.is_err()) continue;
                    jbc::core::AttrBody* body = d.unwrap();
                    if (body == nullptr || body->kind() != jbc::core::AttrKind::Annotations) {
                        continue;
                    }
                    const auto* aa = static_cast<const jbc::core::AnnotationsAttr*>(body);
                    for (const auto& anno : aa->annos) {
                        if (cf.utf8(anno.typeCp) == kKasumiMetadataAnnoDesc) return true;
                    }
                }
                return false;
            };
            annoHit = hasMetadata(cf.attrs);
            if (!annoHit) {
                for (jbc::core::MemberInfo& m : cf.methods) {
                    if (hasMetadata(m.attrs)) {
                        annoHit = true;
                        break;
                    }
                }
            }
            if (!annoHit) {
                for (jbc::core::MemberInfo& f : cf.fields) {
                    if (hasMetadata(f.attrs)) {
                        annoHit = true;
                        break;
                    }
                }
            }
        }
    });

    if (namespaceHit || bsmHit || annoHit) return Era::Kasumi;
    return Era::Unknown;
}

}   // namespace grunt::knowledge
