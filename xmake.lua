-- grunt：Grunt 混淆家族的 deobf 插件仓（mjbase pass-pack + 独立 CLI）。
-- 依赖形态：mjbase 包消费（luiox-repo，0.0.4 起 plugin/pass-pack 接口随包
-- 出仓）；libjsim 源码级（header-only，兄弟目录检出，桥在
-- src/grunt/jsim_bridge.hpp——设计稿 §9：桥由消费方自备）。
--
-- 依赖红线：本仓只 include mjbase/libjbc 公开头 + 自身代码；旧栈与产品线
-- 内容不入仓（含测试与语料）。

-- libjsim：单方法模拟执行库（header-only，纯 std）。grunt 侧只用它跑
-- 「拿真实 CodeUnit 做常量折叠/解密」这一件事。ext 是扩展包目录
-- （str/num/reflect），与核心同 include 前缀，按需 include。
local libjsim_dir = os.getenv("LIBSIM_DIR")
                    or path.join(os.scriptdir(), "..", "libjsim")
libjsim_dir = path.absolute(libjsim_dir)

-- 版本单源；数值与 luiox 家族各仓同一字面量（嵌套 libca 实例统一）。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"

set_project("grunt")
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

-- mjbase 包消费（0.0.5 = libarchive 死面摘除，包面不再携带 archive）。包面
-- 依赖已与源仓根 require 清单对齐（libjbc >=0.4.22 / libjstd / libjhasm
-- >=0.3.3 / libjstr / libjzip / libgarena / libjclassfile / zlib）。grunt 源码
-- 直接 include 的三面各按消费面显式声明：mjbase/* → mjbase 包；jbc/* →
-- libjbc（裸下界不低于 mjbase 包面，同一消费图归并解析到同一实例）；
-- libca/* → libca（顶层钉版钉模块集，全图归并单实例——家族各包定义的
-- 嵌套 libca 已统一为同一超集 modules，版本经顶层钉定收敛）。
add_requires("mjbase 0.0.5")
add_requires("libjbc >=0.4.23")
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("mjbase.libca",     {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbc.libca",     {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("mjbase.libjbc.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})

if has_config("with_tests") then
    add_requires("gtest")
end

target("grunt")
    set_kind("static")
    add_packages("mjbase", {public = true})
    add_packages("libjbc", "libca", {public = true})
    add_files("src/grunt/**.cpp")
    add_includedirs("src", {public = true})
    add_includedirs(path.join(libjsim_dir, "src"), {public = true})
    add_includedirs(path.join(libjsim_dir, "ext"), {public = true})
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

target("grunt_cli")
    set_kind("binary")
    add_deps("grunt")
    add_packages("mjbase", "libjbc", "libca")
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
    add_packages("gtest", "mjbase", "libjbc", "libca")
    add_files("test/unittest/**.cpp")
    set_rundir("$(projectdir)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
