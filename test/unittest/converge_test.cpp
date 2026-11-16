/// @file        converge_test.cpp
/// @brief       不动点驱动器测试（指纹 + 收敛循环 + 真管线两轮收敛）。
///
/// 覆盖分工：
/// - SNAPSHOT：快照确定性（同容器双拍同哈希）、逐条目差异计数（改写/删除）、
///   manifest 只记存在性不参与内容哈希；
/// - DRIVER（伪 runner）：前 2 轮变第 3 轮起不变 → rounds==3 收敛；轮上限
///   截断如实报告 converged=false；错误透传中止；
/// - DRIVER（真管线）：goto-越过-死块夹具（b.main 族形态的最小形）——
///   第 1 轮 DeadCode 擦死块后 goto 才退化为冗余、第 2 轮穿线、第 3 轮
///   确认收敛；同夹具 maxRounds=1 截断。两轮 build 复用同一 PipelineBuilder
///   （工厂重建语义，converge.hpp 契约）由本用例隐式锁定。
#include "grunt/converge.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"
#include "grunt/pack.hpp"

#include "mjbase/plugin/pack.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/pipeline_builder.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::converge {
namespace {

namespace model = mjbase::model;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        sourceName = utf16(name + ".jasm");
    auto                        text       = utf16(source);
    auto                        assembled  = service.assemble(sourceName.ref(), text.ref());
    if (!assembled.isOk()) {
        std::string detail;
        for (const auto& d : assembled.diagnostics) {
            detail += "\n  " + d.message.to_utf8_string().to_std_string();
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail);
    }
    return std::move(assembled.value->bytes);
}

model::ResourceContainer makeContainer(
    std::initializer_list<std::pair<std::string, std::string>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, source] : classes) {
        const std::string uri = name + ".class";
        container.addResource(
            uri, std::make_unique<model::ClassResource>(uri, assemble(name, source)));
    }
    return container;
}

/// 类字节拷贝（容器只读面；变异走 replaceResource）。
std::vector<ca::u8> classBytesCopy(const model::ResourceContainer& container,
                                   const std::string&              uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

/// 逗号分割主线程序（与 CLI splitNames 同口径）。
std::vector<std::string> splitMainLine(const std::string& list)
{
    std::vector<std::string> out;
    std::string              item;
    std::istringstream       ss(list);
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

/// 产物方法按布局序的 op 列表（夹具单方法，只看指令面）。
std::vector<jbc::core::Op> dumpOps(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    if (promoted.is_err() || promoted.unwrap().get() == nullptr) {
        throw std::runtime_error("产物方法无法 promote");
    }
    std::vector<jbc::core::Op> ops;
    for (const jbc::ir::InsnId id : promoted.unwrap().get()->dom.order()) {
        const jbc::ir::Insn* in = promoted.unwrap().get()->dom.peek(id);
        if (in != nullptr) ops.push_back(in->op);
    }
    return ops;
}

/// 真 SPI 轮驱动：pass 名列表 → builder → runToConvergence。
/// 每轮 buildNestedPipeline（builder 复用，工厂重建）——与 CLI runOnce 同构，
/// 层次图省略（grunt pass 的帧链在 rewrite_tools::FrameContext 内部自建）。
ConvergeResult runConverge(model::ResourceContainer&          container,
                           std::initializer_list<const char*> passes, ca::usize maxRounds,
                           std::FILE* traceOut)
{
    mjbase::spi::PassNameRegistry registry;
    if (mjbase::plugin::installPack(grunt::grunt_pack(), registry).is_err()) {
        throw std::runtime_error("pack 装配失败");
    }
    mjbase::spi::PipelineBuilder builder;
    for (const char* name : passes) {
        if (mjbase::spi::applyTo(registry, name, builder).is_err()) {
            throw std::runtime_error(std::string("pass 装配失败：") + name);
        }
    }
    RoundRunner runOnce = [&container,
                           &builder]() -> ca::core::Result<void, std::string> {
        mjbase::spi::PassJobContext job;
        auto                        gpm = builder.buildNestedPipeline(job, nullptr, nullptr, {});
        if (auto r = mjbase::spi::runContainerPipeline(container, gpm, job); r.is_err()) {
            return ca::core::Err(r.unwrap_err().detail);
        }
        return ca::core::Ok();
    };
    return runToConvergence(runOnce, container, maxRounds, traceOut);
}

// ============================================================================
// ==== SNAPSHOT ==============================================================
// ============================================================================

/// goto-越过-死块夹具（b.main 族形态的最小形）：goto 目标在死块之后——
/// 死块擦除前 goto 不冗余（落点 = 死块头），擦除后才退化为冗余，下一轮
/// DeadCode 穿线。驱动器两轮收敛的活形。
const char* kGotoOverDeadSource =
    ".super java/lang/Object\n"
    ".class public super fixture/CvgHost {\n"
    "  .method public static grab ()I {\n"
    "    code: {\n"
    "      bipush 7\n"
    "      goto Lnext\n"
    "    Ldead:\n"
    "      athrow\n"
    "      bipush 1\n"
    "      ireturn\n"
    "    Lnext:\n"
    "      bipush 5\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 无残面夹具：单方法直线计算，任何 pass 都 no-op。
const char* kCleanSource =
    ".super java/lang/Object\n"
    ".class public super fixture/CvgClean {\n"
    "  .method public static grab ()I {\n"
    "    code: {\n"
    "      bipush 7\n"
    "      bipush 5\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

TEST(ConvergeSnapshot, DeterministicAndDiffCounted)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgHost", kGotoOverDeadSource}});

    const Snapshot first  = snapshot(container);
    const Snapshot second = snapshot(container);
    EXPECT_EQ(first.hash, second.hash) << "同容器双拍必须同哈希（遍历序无关）";
    EXPECT_EQ(diffCount(first, second), 0u);

    // 内容变化：同 uri 换字节（replaceResource last-wins）→ 差异 1。
    std::vector<ca::u8> mutated = classBytesCopy(container, "fixture/CvgHost.class");
    mutated.push_back(0);
    container.replaceResource("fixture/CvgHost.class",
                              std::make_unique<model::ClassResource>("fixture/CvgHost.class",
                                                                     std::move(mutated)));
    const Snapshot after = snapshot(container);
    EXPECT_NE(after.hash, first.hash);
    EXPECT_EQ(diffCount(first, after), 1u);
    EXPECT_EQ(diffCount(after, first), 1u);
}

TEST(ConvergeSnapshot, DeletionCounted)
{
    model::ResourceContainer container = makeContainer(
        {{"fixture/CvgHost", kGotoOverDeadSource}, {"fixture/CvgClean", kCleanSource}});
    const Snapshot before = snapshot(container);
    container.getResources().erase("fixture/CvgClean.class");
    const Snapshot after = snapshot(container);
    EXPECT_EQ(diffCount(before, after), 1u);
}

// ============================================================================
// ==== DRIVER（伪 runner）====================================================
// ============================================================================

/// 前 2 轮改容器、第 3 轮起不动：rounds==3（含确认轮）、converged。
TEST(ConvergeDriver, ConvergesAfterTwoMutationRounds)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgClean", kCleanSource}});

    ca::usize flips = 0;
    RoundRunner flipTwice = [&container, &flips]() -> ca::core::Result<void, std::string> {
        if (flips < 2) {
            std::vector<ca::u8> mutated = classBytesCopy(container, "fixture/CvgClean.class");
            mutated.push_back(static_cast<ca::u8>(flips));
            container.replaceResource("fixture/CvgClean.class",
                                      std::make_unique<model::ClassResource>(
                                          "fixture/CvgClean.class", std::move(mutated)));
            ++flips;
        }
        return ca::core::Ok();
    };

    const ConvergeResult verdict = runToConvergence(flipTwice, container, 12, nullptr);
    EXPECT_TRUE(verdict.converged);
    EXPECT_EQ(verdict.rounds, 3u);
    ASSERT_EQ(verdict.perRoundChanged.size(), 3u);
    EXPECT_EQ(verdict.perRoundChanged[0], 1u);
    EXPECT_EQ(verdict.perRoundChanged[1], 1u);
    EXPECT_EQ(verdict.perRoundChanged[2], 0u);
}

/// 轮上限截断：一直变的 runner 达上限如实报告未收敛。
TEST(ConvergeDriver, MaxRoundsCapReported)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgClean", kCleanSource}});

    RoundRunner alwaysChanges =
        [&container]() -> ca::core::Result<void, std::string> {
        static ca::u8 stamp = 0;
        std::vector<ca::u8> mutated = classBytesCopy(container, "fixture/CvgClean.class");
        mutated.push_back(++stamp);
        container.replaceResource("fixture/CvgClean.class",
                                  std::make_unique<model::ClassResource>(
                                      "fixture/CvgClean.class", std::move(mutated)));
        return ca::core::Ok();
    };

    const ConvergeResult verdict = runToConvergence(alwaysChanges, container, 4, nullptr);
    EXPECT_FALSE(verdict.converged);
    EXPECT_EQ(verdict.rounds, 4u);
    ASSERT_EQ(verdict.perRoundChanged.size(), 4u);
}

/// runner 错误立即中止透传。
TEST(ConvergeDriver, RunnerErrorStopsAndPropagates)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgClean", kCleanSource}});
    RoundRunner failing = []() -> ca::core::Result<void, std::string> {
        return ca::core::Err(std::string("boom"));
    };
    const ConvergeResult verdict = runToConvergence(failing, container, 5, nullptr);
    EXPECT_FALSE(verdict.converged);
    EXPECT_EQ(verdict.error, "boom");
    EXPECT_TRUE(verdict.perRoundChanged.empty());
}

// ============================================================================
// ==== DRIVER（真管线）=======================================================
// ============================================================================

TEST(ConvergeDriver, TwoRoundRealPipelineThreadsLateRedundantGoto)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgHost", kGotoOverDeadSource}});

    // stdout 走 gtest 静默（traceOut=nullptr），断言只看收敛账与产物字节。
    const ConvergeResult verdict = runConverge(container, {"grunt.DeadCode"}, 12, nullptr);
    EXPECT_TRUE(verdict.converged) << "真管线必须收敛（无病态震荡面）";
    EXPECT_EQ(verdict.rounds, 3u)
        << "第 1 轮擦死块、第 2 轮穿线退化 goto、第 3 轮确认；账："
        << verdict.perRoundChanged.size();

    // goto 物理消失，计算序列原样保留（jasm 把 bipush 规范化成 ldc，既有
    // dead_code_test 注记同源）。
    const std::vector<jbc::core::Op> ops =
        dumpOps(classBytesCopy(container, "fixture/CvgHost.class"));
    EXPECT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::LDC);
    EXPECT_EQ(ops[1], jbc::core::Op::LDC);
    EXPECT_EQ(ops[2], jbc::core::Op::IADD);
    EXPECT_EQ(ops[3], jbc::core::Op::IRETURN);
}

TEST(ConvergeDriver, MaxRoundsCapOnRealPipeline)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgHost", kGotoOverDeadSource}});

    // 第 1 轮擦死块后即被截断：converged=false，但第 1 轮成果已在容器里。
    const ConvergeResult verdict = runConverge(container, {"grunt.DeadCode"}, 1, nullptr);
    EXPECT_FALSE(verdict.converged);
    EXPECT_EQ(verdict.rounds, 1u);
    EXPECT_EQ(verdict.error, "");
}

TEST(ConvergeDriver, CleanFixtureSingleConfirmRound)
{
    model::ResourceContainer container = makeContainer({{"fixture/CvgClean", kCleanSource}});

    const ConvergeResult verdict = runConverge(container, {"grunt.DeadCode"}, 12, nullptr);
    EXPECT_TRUE(verdict.converged);
    EXPECT_EQ(verdict.rounds, 1u);
    ASSERT_EQ(verdict.perRoundChanged.size(), 1u);
    EXPECT_EQ(verdict.perRoundChanged[0], 0u);
}

/// 默认主线装配探针：defaultMainLine 全部 pass 名可装配（fail loud 早于跑样
/// 本）；builder 一次 build 覆盖全部名字。
TEST(ConvergeDriver, DefaultMainLineAssembles)
{
    const std::vector<std::string> names = splitMainLine(grunt::defaultMainLine());
    EXPECT_FALSE(names.empty());

    mjbase::spi::PassNameRegistry registry;
    ASSERT_FALSE(mjbase::plugin::installPack(grunt::grunt_pack(), registry).is_err());
    mjbase::spi::PipelineBuilder builder;
    for (const std::string& name : names) {
        EXPECT_FALSE(mjbase::spi::applyTo(registry, name, builder).is_err()) << name;
    }
    EXPECT_EQ(builder.globalPassCount(), names.size());
}

}   // namespace
}   // namespace grunt::converge
