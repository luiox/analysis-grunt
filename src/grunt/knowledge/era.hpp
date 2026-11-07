/// @file        knowledge/era.hpp
/// @brief       Grunt 家族知识：版本（Era）与家族常量。
///
/// CONVENTION §6.3：版本分支只允许出现在 scan 的细找里，且必须是 S3 级判据。
/// 本文件是**唯一**允许出现版本相关字面量的地方（字面量本体在
/// kasumi_names.hpp / booger_names.hpp / grunt_names.hpp 的 indy 段，本文件
/// 只做探测裁决）。
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
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/knowledge/kasumi_names.hpp"

namespace grunt::knowledge {

/// @brief 家族版本。
enum class Era : ca::u8 {
    Unknown = 0,  ///< 判据不足：fail-closed，不做猜测性改写
    Grunt2,       ///< Gruntpocalypse 2.x（判据仍缺，如实挂账——见 probeEra 注）
    Grunt3,       ///< Grunteon（`@Metadata` 注解 + Grunt BSM desc 双指纹）
    Kasumi,       ///< Kasumi Obfuscator（indy 全量改写 + `@Metadata` + 反 LLM 资源）
};

/// @brief 版本探测（每家族仓固定提供这一个函数）。命中即判：三个 Kasumi
///        指纹任一命中即 `Era::Kasumi`，Grunt3 双指纹同时命中即
///        `Era::Grunt3`（混淆 jar 由单一混淆器版本产出，不做加权投票）；
///        全部落空返回 `Unknown`。
///
/// - **Kasumi 命名空间**：容器内有 `dev/kasumi/obfuscator` 前缀的类（混淆器
///   运行时类必然在场——Main-Class 即在其中，25 类样本全貌见 notes §1）；
/// - **Kasumi 8 参 BSM 指纹**：任一 BootstrapMethods 条目的 MethodHandle
///   方法描述符等于 `kKasumiBootstrapDesc`（Kasumi 的 indy 形态唯一签名）；
/// - **Kasumi `@Metadata` 注解**：类/字段/方法任一挂载点带
///   `@Ldev/kasumi/internal/Metadata;`（每类一个，Kasumi 特有）。
/// - **Grunt3 `@Metadata` 指纹**：类级可见注解类型等于
///   `kGrunt3MetadataAnnoDesc`（`Lnet/spartanb312/grunteon/internal/Metadata;`，
///   v3 默认 metadataClass——`grunteon` 字样 v2 不产，v2 默认是
///   `net/spartanb312/grunt/GruntMetadata`）；
/// - **Grunt3 命名空间指纹**：容器内有 `net/spartanb312/grunteon/` 前缀的
///   类（v3 现场生成的注解类与 NativeLoader 都落在此命名空间）；
/// - **Grunt BSM desc 指纹**：任一 BootstrapMethods 条目的 MethodHandle
///   方法描述符等于 `kGruntSimpleBsmDesc` 或 `kGruntHeavyBsmDesc`
///   （v2/v3 两代逐字同源，只证明「Grunt 家族 indy 机器在场」）。
///
/// `Era::Grunt3` = Grunt 家族指纹（BSM desc）**且**（`@Metadata` 注解或
/// grunteon 命名空间）——双半边钉版本，单一半边不判。
///
/// **Grunt2 判据仍缺，如实挂账**：`U+200E` 尾缀两代同源、调用点模板无稳定
/// 差异、indy BSM desc 两代逐字同源——当前没有任何「只在 v2 产物出现」的
/// 稳定指纹（v2 默认 `GruntMetadata` 注解名可配置、且 heavy 关闭时注解类
/// 不生成，不能当判据）。v2 专面出现需求时再补（缺口 #15 的 v2 半边）。
///
/// `constant_call` 不消费 Era：它的判据是语义事实（纯函数 + 常量实参 +
/// 求值成功），与混淆器版本无关。`string_pool_fold` 不消费 Era（v2/v3 形态
/// 是配置不是版本键）。`indy_restore` 消费口径：simple 面按 BSM 参数结构
/// 自证（key 静态在场 + 解密产物与 indy desc 互证，不挂 Era 门——理由见
/// doc/indy-restore.md）；heavy 面只在本函数判出 `Era::Grunt3` 时激活
/// （`@Metadata` 数组的解释依赖 v3 语义：mv 门 + `data1 % 59557 == magic2`
/// 自证模数是两代同源，但注解指纹半边只有 v3 在场，故按保守口径只放 v3）。
inline Era probeEra(const mjbase::model::ResourceContainer& container)
{
    bool namespaceHit  = false;
    bool bsmHit        = false;
    bool annoHit       = false;
    bool grunt3Anno    = false;
    bool grunt3Bsm     = false;
    bool grunt3Space   = false;

    forEachClass(container, [&](const ClassEntry& entry) {
        if (namespaceHit && bsmHit && annoHit && grunt3Anno && grunt3Bsm && grunt3Space) {
            return;
        }
        auto session = openSession(*entry.bytes);
        if (!session) return;

        const jbc::ir::ClassUnit& unit = session->unit();
        const std::string_view    self = unit.symbols().str(unit.thisClass);
        if (!namespaceHit && self.rfind(kKasumiNamespace, 0) == 0) namespaceHit = true;
        if (!grunt3Space && self.rfind(kGrunt3NamespacePrefix, 0) == 0) grunt3Space = true;

        // ---- 8 参 BSM 指纹：BootstrapMethods 条目 → MethodHandle → desc ----
        if (!bsmHit || !grunt3Bsm) {
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
                const std::string_view desc = cf.utf8(nat.b);
                if (desc == kKasumiBootstrapDesc) bsmHit = true;
                if (desc == kGruntSimpleBsmDesc || desc == kGruntHeavyBsmDesc) {
                    grunt3Bsm = true;
                }
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
                            if (bsmHit && grunt3Bsm) break;
                        }
                    }
                }
            }
        }

        // ---- @Metadata 注解：类/字段/方法三张挂载面（对齐 booger 普查口径）----
        // Kasumi 判三类挂载面；Grunt3 只判类级可见注解（v3 生成面只挂类级）。
        if (!annoHit || !grunt3Anno) {
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
                        if (cf.utf8(anno.typeCp) == kKasumiMetadataAnnoDesc) annoHit = true;
                        if (cf.utf8(anno.typeCp) == kGrunt3MetadataAnnoDesc) grunt3Anno = true;
                    }
                }
            };
            hasMetadata(cf.attrs);
            if (!annoHit || !grunt3Anno) {
                for (jbc::core::MemberInfo& m : cf.methods) {
                    hasMetadata(m.attrs);
                    if (annoHit && grunt3Anno) break;
                }
            }
            if (!annoHit || !grunt3Anno) {
                for (jbc::core::MemberInfo& f : cf.fields) {
                    hasMetadata(f.attrs);
                    if (annoHit && grunt3Anno) break;
                }
            }
        }
    });

    if (namespaceHit || bsmHit || annoHit) return Era::Kasumi;
    if (grunt3Bsm && (grunt3Anno || grunt3Space)) return Era::Grunt3;
    return Era::Unknown;
}

}   // namespace grunt::knowledge
