/// @file        main.cpp
/// @brief       grunt CLI：装 pack → 导 jar → 管线 → 导 jar。
///
/// 各插件仓自组 CLI：mjbase 出框架（注册表/管线/容器 IO/workspace 装配），
/// 本仓只出 pack 与残留实现。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "grunt/pack.hpp"

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/log/log_init.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/plugin/pack.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace {

constexpr const char* kUsage =
    "grunt_cli -i in.jar -o out.jar -p grunt.X[,grunt.Y...] [-c pack.json]\n"
    "  -i  输入 jar\n"
    "  -o  输出 jar\n"
    "  -p  逗号分隔 pass 名（grunt. 前缀）\n"
    "  -c  pack 配置 JSON 文件（可选，文本整体交给 pack 解析）\n"
    "  --list  列出本 pack 注册的全部 pass 后退出\n";

std::string readFileText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> splitNames(const std::string& list)
{
    std::vector<std::string> out;
    std::string              item;
    std::istringstream       ss(list);
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}   // namespace

int main(int argc, char** argv)
{
    mjbase::log::initDefaultLogger();

    std::string input;
    std::string output;
    std::string passList;
    std::string configPath;
    bool        list = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            list = true;
        } else if (arg == "-i" && i + 1 < argc) {
            input = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            passList = argv[++i];
        } else if (arg == "-c" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }

    std::string configText;
    if (!configPath.empty()) {
        configText = readFileText(configPath);
        if (configText.empty()) {
            std::fprintf(stderr, "配置文件为空或不可读：%s\n", configPath.c_str());
            return 2;
        }
    }

    mjbase::spi::PassNameRegistry registry;
    if (ca::core::Result<void, mjbase::plugin::PackError> r =
            mjbase::plugin::installPack(grunt::grunt_pack(), registry, configText);
        r.is_err()) {
        std::fprintf(stderr, "pack 装配失败：%s：%s\n",
                     mjbase::plugin::to_string(r.unwrap_err().kind),
                     r.unwrap_err().detail.c_str());
        return 2;
    }

    if (list) {
        for (const auto& entry : registry.list()) {
            if (entry.name.substr(0, 6) == "grunt.") {
                std::printf("%.*s\t%.*s\n", static_cast<int>(entry.name.size()), entry.name.data(),
                            static_cast<int>(entry.description.size()),
                            entry.description.data());
            }
        }
        return 0;
    }

    if (input.empty() || output.empty() || passList.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    mjbase::spi::PipelineBuilder builder;
    for (const std::string& name : splitNames(passList)) {
        if (auto r = mjbase::spi::applyTo(registry, name, builder); r.is_err()) {
            std::fprintf(stderr, "pass 装配失败：%s（%s）\n", name.c_str(),
                         mjbase::spi::to_string(r.unwrap_err()));
            return 2;
        }
    }

    mjbase::model::ResourceContainer container;
    if (auto m = mjbase::model::ResourceHelper::importFromJar(container, input); m.is_err()) {
        std::fprintf(stderr, "jar 导入失败（%s）：%s\n", mjbase::model::to_string(m.unwrap_err()),
                     input.c_str());
        return 2;
    }

    mjbase::spi::PassJobContext job;

    // 帧链接线（pass-contract §9：resolver 数据获取是调用方的事）：本 jar 的类
    // 登记成层次图，Strict resolver 注入管线选项。折叠会改变方法体长度，既有
    // StackMapTable 必须整体重算——未注入 resolver 时管线只做 relocation。
    mjbase::KeyPool                                   pool;
    mjbase::hierarchy::ClassHierarchy                 hierarchy(pool);
    const std::vector<mjbase::analysis::ScannedClass> scanned =
        mjbase::analysis::scanClassResources(container);
    for (const mjbase::analysis::ScannedClass& sc : scanned) {
        if (!sc.session) continue;
        (void)hierarchy.addClass(mjbase::hierarchy::fillSummary(*sc.session, pool));
    }
    mjbase::spi::CommonSuperClassResolver  superResolver(hierarchy);
    mjbase::spi::StrictTypeResolverAdapter typeResolver(superResolver);
    mjbase::spi::PassPipelineOptions       options;
    options.typeResolver = &typeResolver;

    auto gpm = builder.buildNestedPipeline(job, nullptr, nullptr, options);
    if (auto r = mjbase::spi::runContainerPipeline(container, gpm, job); r.is_err()) {
        std::fprintf(stderr, "管线失败（%s@%s）：%s\n",
                     mjbase::spi::to_string(r.unwrap_err().stage),
                     r.unwrap_err().target.c_str(), r.unwrap_err().detail.c_str());
        return 3;
    }

    if (auto m = mjbase::model::ResourceHelper::exportToJar(container, output); m.is_err()) {
        std::fprintf(stderr, "jar 导出失败（%s）：%s\n", mjbase::model::to_string(m.unwrap_err()),
                     output.c_str());
        return 2;
    }
    return 0;
}
