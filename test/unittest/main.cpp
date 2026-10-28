/// @file        main.cpp
/// @brief       grunt 单测入口。
///
/// 夹具一律现场汇编，不依赖任何资源目录，所以不需要 mjbase 那套 test_helper
/// 的工程根注册。
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
