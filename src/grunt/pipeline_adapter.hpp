/// @file        pipeline_adapter.hpp
/// @brief       对接 mjbase 管线的适配层：把残留的三段包装成一个 GlobalPass。
///
/// CONVENTION §9：deobf 核心零 pipeline 知识，编排词汇只活在本文件。
#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace grunt {

/// @brief `grunt.ConstantCall`：常量调用折叠的管线入口。
///
/// 三段编排固定：scan → rewrite（有站点才跑）→ proof；proof 失败只报告，
/// 不回滚（回滚由容器写回粒度保证：rewrite 整类成功才写回）。
class ConstantCallPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.FlagsStrip`：伪造访问标志清除的管线入口。
class FlagsStripPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.ConstMethod`：单常量方法内联的管线入口。
class ConstMethodPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.TrampolineSqueeze`：「跳到下一条」goto 蹦床 NOP 化。
class TrampolineSqueezePass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.PredicateFold`：常量操作数分支/switch 静态裁决。
class PredicateFoldPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.DeadCode`：不可达指令码清理。
class DeadCodePass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.ConstValuePromote`：clinit 单点常量写入回填 ConstantValue。
class ConstValuePromotePass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.SeedStrip`：clinit int[] 种子建表拆除。
class SeedStripPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.DeadMember`：零引用方法/字段/空 clinit 删除（含 CP 压缩）。
class DeadMemberPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.BoogerAnnoStrip`：@Booger 运行时注解剥离。
class BoogerAnnoStripPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.IndyFold`：invokedynamic 解密折叠（跑 BSM 求目标 → 真 invoke）。
class IndyFoldPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.NumberFold`：藏形数值常量还原（XOR 隐匿/基 32 长整/位形包装）。
class NumberFoldPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.ProxyInline`：代理方法调用点改指真实目标（内联后死码交下游）。
class ProxyInlinePass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace grunt
