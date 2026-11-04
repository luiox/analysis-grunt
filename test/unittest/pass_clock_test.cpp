/// @file        pass_clock_test.cpp
/// @brief       pass 级 wall-clock 时间片的门与挂起测试（哑类夹具，不汇编）。
///
/// 时间是物理量，测试不靠睡眠：用 forceExpire 测试钩子做确定性过期，
/// 语义面（门放行/拒绝、首停点名、逐类计数、proof 类数门挂起、不设防）
/// 全部走真路径。
#include "grunt/pass_clock.hpp"

#include "libca/core/tag_cast.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace grunt {
namespace {

namespace model = mjbase::model;

/// 哑类夹具：forEachClass 只认「非空 ClassResource」，不需要合法类字节。
void addDummy(model::ResourceContainer& container, const std::string& name)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE}));
}

/// 三类容器，资源序 a/b/c。
model::ResourceContainer threeJars()
{
    model::ResourceContainer container;
    addDummy(container, "a");
    addDummy(container, "b");
    addDummy(container, "c");
    return container;
}

/// 记下 forEachClass 实际下发的 uri 序。
std::vector<std::string> visited(const model::ResourceContainer& container)
{
    std::vector<std::string> seen;
    forEachClass(container, [&](const ClassEntry& e) {
        seen.emplace_back(e.uri);
    });
    return seen;
}

TEST(PassClockTest, NoScopeAdmitsAll)
{
    const model::ResourceContainer container = threeJars();
    pass_clock::gActiveClock = nullptr;
    const std::vector<std::string> seen = visited(container);
    ASSERT_EQ(seen.size(), ca::usize{3});
}

TEST(PassClockTest, UnexpiredScopeAdmitsAll)
{
    const model::ResourceContainer container = threeJars();
    pass_clock::PassScope          scope("grunt.Test", 0);   ///< 0 = 不设防
    const std::vector<std::string> seen = visited(container);
    ASSERT_EQ(seen.size(), ca::usize{3});
    EXPECT_EQ(scope.clock().skipped(), ca::usize{0});
}

TEST(PassClockTest, ExpiredScopeStopsDeliveryAndCounts)
{
    model::ResourceContainer      container = threeJars();
    pass_clock::PassScope         scope("grunt.Test", 1);
    scope.clock().forceExpire();   ///< 确定性过期：从第一个类起就到墙
    const std::vector<std::string> seen = visited(container);
    EXPECT_TRUE(seen.empty());
    EXPECT_EQ(scope.clock().skipped(), ca::usize{3});
}

TEST(PassClockTest, ExpireMidwaySkipsOnlyRest)
{
    model::ResourceContainer container = threeJars();
    {
        pass_clock::PassScope scope("grunt.Test", 1);
        // 先正常放行一个类（模拟超墙前已处理的类），再钉死过期。
        std::vector<std::string> seen;
        forEachClass(container, [&](const ClassEntry& e) {
            seen.emplace_back(e.uri);
            scope.clock().forceExpire();   ///< 第一类处理完即到墙
        });
        // a 已下发并处理；b/c 被门拒收。
        ASSERT_EQ(seen.size(), ca::usize{1});
        EXPECT_EQ(seen[0], "a.class");
        EXPECT_EQ(scope.clock().skipped(), ca::usize{2});
    }
}

TEST(PassClockTest, CountClassesIgnoresExpiredClock)
{
    model::ResourceContainer container = threeJars();
    pass_clock::PassScope    scope("grunt.Test", 1);
    scope.clock().forceExpire();
    // 类数不增门（proof 面）必须看全量：挂起期 forEachClass 不受超墙影响。
    EXPECT_EQ(countClasses(container), ca::usize{3});
    EXPECT_EQ(scope.clock().skipped(), ca::usize{0});   ///< 挂起期不记跳过账
}

TEST(PassClockTest, AdmitNamesFirstBlockedOnly)
{
    model::ResourceContainer  container = threeJars();
    pass_clock::PassScope     scope("grunt.Test", 1);
    scope.clock().forceExpire();
    // 直接打门：首停类被点名记录（skipped 计数逐类累加，首停语义不变）。
    EXPECT_FALSE(scope.clock().admit("b.class"));
    EXPECT_FALSE(scope.clock().admit("c.class"));
    EXPECT_EQ(scope.clock().skipped(), ca::usize{2});
}

}   // namespace
}   // namespace grunt
