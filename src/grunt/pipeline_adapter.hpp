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

}   // namespace grunt
