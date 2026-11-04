/// @file        main.cpp
/// @brief       grunt CLI：装 pack → 导 jar → 管线 → 导 jar。
///
/// 各插件仓自组 CLI：mjbase 出框架（注册表/管线/容器 IO/workspace 装配），
/// 本仓只出 pack 与残留实现。
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#endif

#include "grunt/pack.hpp"
#include "grunt/pipeline_adapter.hpp"

#include "cli/argv_utf8.hpp"
#include "cli/sim_probe.hpp"

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

/// @brief 罐级 fail-closed 的进程收口：未处理 SEH（堆损坏类硬崩——崩点在
/// ntdll 堆管理器内部，catch(...) 既接不住也续不得）在此点名罐与崩时 pass，
/// 以 rc=3 终止。只写字面量到 fd=2，不碰堆（此刻堆不可信）。
const char* gCrashInput = nullptr;

#if defined(_WIN32)
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
    char        buf[256];
    const LONG  code = ep != nullptr && ep->ExceptionRecord != nullptr
                           ? ep->ExceptionRecord->ExceptionCode
                           : 0;
    const char* pass = grunt::gActivePass != nullptr ? grunt::gActivePass : "?";
    const char* jar  = gCrashInput != nullptr ? gCrashInput : "?";
    const int   n = _snprintf(buf, sizeof(buf) - 1,
                             "罐级 fail-closed：硬崩（SEH %08lX @ pass %s）：%s\n",
                             static_cast<unsigned long>(code), pass, jar);
    if (n > 0) _write(2, buf, n);
    ExitProcess(3);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

constexpr const char* kUsage =
    "grunt_cli -i in.jar -o out.jar -p grunt.X[,grunt.Y...] [-c pack.json]\n"
    "  -i  输入 jar\n"
    "  -o  输出 jar\n"
    "  -p  逗号分隔 pass 名（grunt. 前缀）\n"
    "  -c  pack 配置 JSON 文件（可选，文本整体交给 pack 解析）\n"
    "  --list  列出本 pack 注册的全部 pass 后退出\n"
    "  --sim 类uri#方法名   对真方法跑一次模拟执行后退出（调试用，只读）\n"
    "  --args v1,v2,...     --sim 的实参：十进制 / s:文本 / x:十六进制 / b:装箱整数 / u:（Unknown）\n"
    "  --exec               --sim 走跨方法执行器（子方法调用自动展开）；默认单方法裸跑\n"
    "  --dump               --sim 打印桥产物的反汇编视图（只读）\n"
    "  环境变量 GRUNT_SIM_TRACE=1 时打印逐步栈形\n";

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

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crashFilter);
#endif

    mjbase::log::initDefaultLogger();

    // argv 编码根修：Windows 取宽字符 argv 转 UTF-8（窄字符 main 的 ANSI argv
    // 对非 ASCII 罐名必然错配）；POSIX 直通。
    std::vector<std::string> args;
#if defined(_WIN32)
    if (!grunt::cli::utf8Args(argc, argv, args)) {
        std::fprintf(stderr, "argv 含不可编码字符（UTF-8 转换失败）\n%s", kUsage);
        return 2;
    }
#else
    grunt::cli::utf8Args(argc, argv, args);
#endif

    std::string input;
    std::string output;
    std::string passList;
    std::string configPath;
    std::string simTarget;
    std::string simArgs;
    bool        simExec = false;
    bool        simDump = false;
    bool        list = false;

    for (ca::usize i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--list") {
            list = true;
        } else if (arg == "--sim" && i + 1 < args.size()) {
            simTarget = args[++i];
        } else if (arg == "--args" && i + 1 < args.size()) {
            simArgs = args[++i];
        } else if (arg == "--exec") {
            simExec = true;
        } else if (arg == "--dump") {
            simDump = true;
        } else if (arg == "-i" && i + 1 < args.size()) {
            input = args[++i];
        } else if (arg == "-o" && i + 1 < args.size()) {
            output = args[++i];
        } else if (arg == "-p" && i + 1 < args.size()) {
            passList = args[++i];
        } else if (arg == "-c" && i + 1 < args.size()) {
            configPath = args[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }

    // 探针是独立通路：不进 pack、不进管线，只读容器跑一次模拟执行。
    if (!simTarget.empty()) {
        if (input.empty()) {
            std::fputs("--sim 需要 -i 指定输入 jar\n", stderr);
            return 2;
        }
        grunt::cli::SimProbeOptions probe;
        probe.jarPath  = input;
        probe.target   = simTarget;
        probe.argsSpec = simArgs;
        probe.executor = simExec;
        probe.dump     = simDump;
        return grunt::cli::runSimProbe(probe);
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

    gCrashInput = input.c_str();

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

    // 罐级 fail-closed：框架段（层次图装配/管线驱动/导出）的异常与 SEH 硬崩
    // 不许无声穿透——pass 段已由 FailClosedPass 单点收口，这里是最后一道。
    try {
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
    } catch (const std::exception& e) {
        std::fprintf(stderr, "罐级 fail-closed：异常穿透（%s）：%s\n", e.what(), input.c_str());
        return 3;
    } catch (...) {
        std::fprintf(stderr, "罐级 fail-closed：硬崩（SEH 穿透）：%s\n", input.c_str());
        return 3;
    }
    return 0;
}
