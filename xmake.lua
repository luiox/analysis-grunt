-- grunt：Grunt 混淆家族的 deobf 插件仓（mjbase pass-pack + 独立 CLI）。
-- 依赖形态：全包消费（mjbase / libjsim 均 luiox-repo 包）；桥走 libjsim
-- 官方 jsim/jbc.hpp（jsim::jbcbridge::toSimMethod，自建 jsim_bridge 已退役）。
--
-- 依赖红线：本仓只 include mjbase/libjbc/libjsim 公开头 + 自身代码；旧栈与
-- 产品线内容不入仓（含测试与语料）。

-- libjsim：单方法模拟执行库（header-only，纯 std），luiox-repo 包消费。
-- grunt 侧只用它跑拿真实 CodeUnit 做常量折叠/解密这一件事。

-- 版本单源；数值与 luiox 家族各仓同一字面量（嵌套 libca 实例统一）。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"

set_project("grunt")
set_version("0.1.0")
set_xmakever("2.9.3")
set_allowedplats("windows", "linux", "macosx")
set_defaultplat("windows")
set_defaultarchs("x64")
set_languages("c99", "cxx17")
add_rules("mode.debug", "mode.release")
set_warnings("all")

if is_plat("windows") then
    set_runtimes("MT")
    set_toolchains("msvc")
    add_cxflags("/GR-", {tools = "cl"})
    add_cxflags("/EHa", {tools = "cl"})
    add_cxflags("/wd4541", {tools = "cl"})
    set_exceptions("none")
else
    add_cxxflags("-fno-rtti")
end

add_defines("CA_COMPILE_LOG_LEVEL=0")

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable grunt_unittest (pulls gtest via xrepo).")
option_end()

add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")

-- mjbase 包消费（0.0.5 = libarchive 死面摘除；0.0.18 = jar 导入 Dup 名末现
-- 保留对齐 JVM 类加载口径 #160 + 注解递归畸形输入边界守卫 #158）。包面
-- 依赖已与源仓根 require 清单对齐（libjbc >=0.4.22 / libjstd / libjhasm
-- >=0.3.3 / libjstr / libjzip / libgarena / libjclassfile / zlib）。grunt 源码
-- 直接 include 的四面各按消费面显式声明：mjbase/* → mjbase 包；jbc/* →
-- libjbc（裸下界 >=0.4.24 = 与 libjsim 桥面下界的交集归一，防实例分裂，
-- 同一消费图归并解析到同一实例）；libca/* → libca（顶层钉版钉模块集，
-- 全图归并单实例，家族各包定义的嵌套 libca 已统一为同一超集 modules，
-- 版本经顶层钉定收敛）；jsim/* → libjsim 包（jbc 桥 config 开，桥迁官方
-- jsim/jbc.hpp，libjbc 随桥面进 jsim 消费图，与 grunt 直连面同实例归并）。
add_requires("mjbase 0.0.18")
add_requires("libjbc >=0.4.24")
add_requires("libjsim 0.0.1", {configs = {jbc = true}})
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("mjbase.libca",     {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbc.libca",     {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("mjbase.libjbc.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjsim.libjbc.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})

if has_config("with_tests") then
    add_requires("gtest")
end

target("grunt")
    set_kind("static")
    add_packages("mjbase", {public = true})
    add_packages("libjbc", "libca", "libjsim", {public = true})
    add_files("src/grunt/**.cpp")
    add_includedirs("src", {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("grunt_cli")
    set_kind("binary")
    add_deps("grunt")
    add_packages("mjbase", "libjbc", "libca", "libjsim")
    add_files("src/cli/**.cpp")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("grunt_unittest")
    set_kind("binary")
    if not has_config("with_tests") then
        set_enabled(false)
    end
    add_deps("grunt")
    add_packages("gtest", "mjbase", "libjbc", "libca", "libjsim")
    add_files("test/unittest/**.cpp")
    -- CLI argv 转换单测：转换函数是 cli/ 下的独立编译单元，与被测面一致。
    add_files("src/cli/argv_utf8.cpp")
    set_rundir("$(projectdir)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
