/// @file        pipeline_adapter.hpp
/// @brief       对接 mjbase 管线的适配层：把残留的三段包装成一个 GlobalPass。
///
/// CONVENTION §9：deobf 核心零 pipeline 知识，编排词汇只活在本文件。
#pragma once

#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <unordered_set>

#include "grunt/pass_clock.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace grunt {

/// @brief 当前正在执行的 pass 名（崩溃过滤器点名用；管线外为空）。
extern const char* gActivePass;

/// @brief `grunt.IndyDecode` 的 JDK 接口旁证注入面：CLI 在管线启动前填 JDK
/// 根目录（`--jdk`）。文件系统副作用只发生在这里（adapter 层）——残留本体
/// 不做任何遍历；空串 = 不注入，interface 路判 fail-closed。
extern std::string gKasumiJdkPath;

/// @brief `grunt.StringCacheFold` 的 TSV 旁证注入面：CLI 在管线启动前填
/// KasumiStringDumper 产出的明文表路径（`--tsv`）。文件读取只发生在这里
/// （adapter 层）；空串 = 不注入，旁证路判 fail-closed（Absent）。
extern std::string gKasumiTsvPath;

/// @brief 读 TSV 文本文件。@return 是否可读（内容进 out，UTF-8 直通——解析
///        在 string_cache_fold::parseTsv 纯函数面）。
bool readTextFile(const std::string& path, std::string& out);

/// @brief 扫描 JDK（jmods/ 优先，lib/ 兜底）收集接口内部名（ACC_INTERFACE）。
/// 读不了的归档跳过；结果可能为空（调用方按无旁证口径处理）。
std::unordered_set<std::string> scanJdkInterfaces(const std::string& jdkPath);

/// @brief fail-closed 装饰器：pass 体抛出的异常（含 /EHa 下的 SEH 硬崩）不再
/// 穿透管线——stderr 点名后按「本 pass 产出丢弃」收场，罐不拖垮整轮语料。
///
/// 口径对齐既有保守面：崩在哪一 pass 就丢哪一 pass 的产出，容器保有此前
/// 已落盘状态；PreservedAnalyses 全弃（失效面最大，宁枉勿纵）。
template <class Inner>
class FailClosedPass final : public mjbase::spi::GlobalPass
{
public:
    explicit FailClosedPass(const char* name) : name_(name) {}

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override
    {
        struct Scoped
        {
            const char* prev;
            Scoped(const char* n) : prev(gActivePass) { gActivePass = n; }
            ~Scoped() { gActivePass = prev; }
        } scoped(name_);
        // pass 级 wall-clock 时间片（与 iter_budget 类级预算互补：类级封迭代
        // 尾，pass 级封时间墙）。超墙后 forEachClass 对余下类保守止步。
        const pass_clock::PassScope slice(name_);
        try {
            return inner_.run(container, am);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[grunt] fail-closed：%s 异常穿透（%s），本 pass 产出丢弃\n",
                         name_, e.what());
        } catch (...) {
            std::fprintf(stderr, "[grunt] fail-closed：%s 硬崩（SEH 穿透），本 pass 产出丢弃\n",
                         name_);
        }
        return mjbase::spi::PreservedAnalyses::none();
    }

private:
    Inner       inner_;
    const char* name_;
};

/// @brief 注册点统一入口：所有 pass 经此成 pass，异常面单点收口。
template <class Inner>
std::unique_ptr<mjbase::spi::GlobalPass> makeFailClosed(const char* name)
{
    return std::make_unique<FailClosedPass<Inner>>(name);
}

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

/// @brief `grunt.FieldAccessProxy`：字段代理调用点改回真实字段访问（死码交下游）。
class FieldAccessProxyPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.IndyDecode`：Kasumi indy 静态还原（key 提取 + bsmArgs 解密 +
/// 7 形态改写；Era=Kasumi 判定 + JDK 接口旁证注入见 gKasumiJdkPath）。
class IndyDecodePass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.StringCacheFold`：Kasumi 字符串缓存解密结果回填常量池
/// （两形态改写；Era=Kasumi 判定 + TSV 明文旁证注入见 gKasumiTsvPath）。
class StringCacheFoldPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.StringPoolFold`：Grunt v2/v3 字符串池读点折回 ldc 明文
/// （池链结构自证判据，无外部旁证注入面；Era 不消费——形态内无版本敏感判据）。
class StringPoolFoldPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// @brief `grunt.MetadataStrip`：Kasumi 元数据残留清理（@Metadata 注解三面
/// 摘除 + 水印字段删除 + 反 LLM 资源删除；Era=Kasumi 判定，三面判据全在
/// scan 的 Era 分支内）。
class MetadataStripPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace grunt
