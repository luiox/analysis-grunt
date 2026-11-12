/// @file        cff_unflatten_test.cpp
/// @brief       残留 `cff_unflatten` 的三段判据测试（真值往返 + 拒绝面反证 +
///              JVM -Xverify:all 实跑）。
///
/// 夹具是缩小版 Grunt v3 ControlflowFlattening 产物形态（jasm 现场汇编，
/// 源算法锚 grunt3@e41e852 FlowControlFlowFlattener.kt，boar-main.jar
/// `net/spartanb312/obf/AD.a` 验证同构）：每方法一个 fresh state 槽、每出口
/// 一个 dispatcher 岛 `[ILOAD s; TABLESWITCH]`（键窗连续、default 落岛首真
/// 块 = alias face）、每条入边一个常量供给桥（inline span 闭链）、junk 检查
/// 与 scramble 假桥同形。
///
/// 真值往返的证据链：桥 span 的求值（Add/Xor 闭链）与键表（tableswitch 静
/// 态编码）是同一份静态事实的两半，scan 反查桥 → 真块，rewrite 直连后
/// 方法恢复直线块序；断言反摊平后的 JVM 实跑结果 == 夹具真值表写下的值
/// （无循环论证：判据不读方法名，键表由结构重建）。
///
/// 覆盖分工：
/// - SCAN：单岛 / 多岛同槽 / 边载体 junk 检查 / 异常 handler 桥（per-frame
///   第二岛）四形态定案；state 槽越界写、processor 形桥、span 除零、岛间
///   共享块、InvokeDispatcher 方法头（反误伤）、异常 start 锚被擦块 →
///   拒绝面逐桶计数；
/// - REWRITE：岛 + 桥擦除、alias-face 剪枝（scramble → default）、入口块归
///   位、handler 重绑、无假臂 guard紧邻岛头的 positional 落空臂钉；拒绝面上
///   容器字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零（未跑 rewrite 时如实
///   报本可反摊平岛数）；
/// - JVM：-Xverify:all 下实跑反摊平产物（含异常路径），stdout 对照真值。
#include "grunt/cff_unflatten.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::cff_unflatten {
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

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                        result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
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

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

// ============================================================================
// ==== 夹具（v3 FlowControlFlowFlattener 产物缩样）===========================
// ============================================================================
//
// flat：单岛方法 (II)I，state 槽 2，键窗 [10,14)。真块 E→B1→B2→R：
//   E  : a = x + y          （key 12 = 入口键）
//   B1 : a = a + 1          （key 10）
//   B2 : a = y*2 + a        （key 11）
//   R  : return a
// flat(3,4) = ((3+4)+1) + 4*2 = 16。
// 桥 span（Add/Xor 闭链）：entry 3^15=12、b1 4+6=10、b2 5+6=11、bR 6+7=13；
// default 落 LE（alias face：LE 同时是 case 12 目标）。scramble 假桥键
// 1+99=100 出表（不可达，rewrite 擦除并剪到 default）。

std::string flatSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat (II)I {\n"
           "    code: {\n"
           "      iconst_3\n"              // 入口桥：3 ^ 15 = 12（E 的键）
           "      bipush 15\n"
           "      ixor\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lscr:\n"                    // scramble 假桥：1+99=100 出表（不可达）
           "      iconst_1\n"
           "      bipush 99\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lb1:\n"                     // B1 的桥：4+6=10
           "      iconst_4\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lb2:\n"                     // B2 的桥：5+6=11
           "      iconst_5\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    LbR:\n"                     // R 的桥：6+7=13
           "      bipush 6\n"
           "      bipush 7\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Ldisp:\n"                   // dispatcher 岛（default = LE，alias face）
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 10,\n"
           "          max: 13,\n"
           "          cases: { LB1, LB2, LE, LR },\n"
           "          default: LE\n"
           "      }\n"
           "    LE:\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto Lb1\n"
           "    LB1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto Lb2\n"
           "    LB2:\n"
           "      iload 1\n"
           "      iconst_2\n"
           "      imul\n"
           "      iload 0\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LbR\n"
           "    LR:\n"
           "      iload 0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 边载体夹具：入口桥的 goto 被 ControlflowJump junk guard 包裹（guard 块无
///        state 读写、唯一前驱 = 常量供给入口桥 → 臂重指 caseOf(12)=LE）。
std::string gateSource(const std::string& name)
{
    std::string src = flatSource(name);
    const std::string needle =
        "      iconst_3\n"
        "      bipush 15\n"
        "      ixor\n"
        "      istore 2\n"
        "      goto Ldisp\n";
    const ca::usize at = src.find(needle);
    if (at == std::string::npos) return src;   // 夹具结构漂移即暴露
    src.replace(at, needle.size(),
                "      iconst_3\n"
                "      bipush 15\n"
                "      ixor\n"
                "      istore 2\n"
                "      goto Lgate\n"          // 入口桥 → junk 检查（不直连岛头）
                "    Lgate:\n"                // 边载体：5<9 恒真 → 岛头；假臂
                "      iconst_5\n"            //   落 junk 尾（原样保留）
                "      bipush 9\n"
                "      if_icmplt Ldisp\n"
                "      iconst_1\n"
                "      ireturn\n");
    // scramble 假桥顺带去掉（本夹具聚焦载体路径）。
    const std::string scr =
        "    Lscr:\n"
        "      iconst_1\n"
        "      bipush 99\n"
        "      iadd\n"
        "      istore 2\n"
        "      goto Ldisp\n";
    const ca::usize sat = src.find(scr);
    if (sat != std::string::npos) src.replace(sat, scr.size(), "");
    return src;
}

/// @brief 异常 handler 桥夹具：真块 B2 除零（100/y），异常区间 [LE, LR)，
///        handler 锚在 handler 桥头（per-frame 第二岛 D2：键窗 [14,15)）。
///        flat(3,4)=33；flat(3,0) 走 handler → -1。
std::string excSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat (II)I {\n"
           "    exceptions: { { LE, LR, Lhb, Ljava/lang/Throwable; } },\n"
           "    code: {\n"
           "      iconst_3\n"              // 入口桥：3^15=12 → E
           "      bipush 15\n"
           "      ixor\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    Lb1:\n"                    // 4+6=10 → B1
           "      iconst_4\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    Lb2:\n"                    // 5+6=11 → B2
           "      iconst_5\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    LbR:\n"                    // 6+7=13 → R
           "      bipush 6\n"
           "      bipush 7\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    Lhb:\n"                    // handler 桥：6+8=14 → H（第二岛 D2）
           "      bipush 6\n"
           "      bipush 8\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp2\n"
           "    Ldisp1:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 10,\n"
           "          max: 13,\n"
           "          cases: { LB1, LB2, LE, LR },\n"
           "          default: LE\n"
           "      }\n"
           "    Ldisp2:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 14,\n"
           "          max: 14,\n"
           "          cases: { LH },\n"
           "          default: LH\n"
           "      }\n"
           "    LE:\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto Lb1\n"
           "    LB1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto Lb2\n"
           "    LB2:\n"
           "      bipush 100\n"            // 100/y：y=0 → ArithmeticException
           "      iload 1\n"
           "      idiv\n"
           "      iload 0\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LbR\n"
           "    LR:\n"
           "      iload 0\n"
           "      ireturn\n"
           "    LH:\n"
           "      pop\n"
           "      iconst_m1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 多岛同槽夹具：两 region 共用 state 槽 2（D1 键窗 [10,12)、D2
///        [20,22)）。flat3(x,y)：E1 a=x+1 → A1 a=a*2 → E2 b=y+3 → A2 a*b。
///        flat3(2,5) = (2+1)*2 * (5+3) = 48。
std::string multiSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat3 (II)I {\n"
           "    code: {\n"
           "      iconst_3\n"              // 入口桥（D1）：3^8=11 → E1
           "      bipush 8\n"
           "      ixor\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    LbA1:\n"                   // 5+5=10 → A1
           "      iconst_5\n"
           "      iconst_5\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    LbE2:\n"                   // 4+18=22 → E2（D2 的入口键桥）
           "      iconst_4\n"
           "      bipush 18\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp2\n"
           "    LbA2:\n"                   // 6+14=20 → A2
           "      bipush 6\n"
           "      bipush 14\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp2\n"
           "    Ldisp1:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 10,\n"
           "          max: 11,\n"
           "          cases: { LA1, LE1 },\n"
           "          default: LE1\n"
           "      }\n"
           "    Ldisp2:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 20,\n"
           "          max: 21,\n"
           "          cases: { LA2, LE2 },\n"
           "          default: LE2\n"
           "      }\n"
           "    LE1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LbA1\n"
           "    LA1:\n"
           "      iload 0\n"
           "      iconst_2\n"
           "      imul\n"
           "      istore 0\n"
           "      goto LbE2\n"
           "    LE2:\n"
           "      iload 1\n"
           "      iconst_3\n"
           "      iadd\n"
           "      istore 1\n"
           "      goto LbA2\n"
           "    LA2:\n"
           "      iload 0\n"
           "      iload 1\n"
           "      imul\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief positional 落空臂钉夹具：junk 检查紧邻岛头（guard 块 if_icmplt 的落空
///        臂是位置边，直落岛头），岛头被擦后落空臂会改落邻接后继（LB1），
///        改写期须补 GOTO 钉回 case 目标（LE）。guard 带假分支时假臂自成一块、
///        不触发此形，故本夹具无假臂且 LB1 紧跟 Ldisp。
std::string pinGateSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat (II)I {\n"
           "    code: {\n"
           "      iconst_3\n"              // 入口桥：3^15=12 → junk 检查（不直连岛头）
           "      bipush 15\n"
           "      ixor\n"
           "      istore 2\n"
           "      goto Lgate\n"
           "    Lb1:\n"                    // 4+6=10 → B1
           "      iconst_4\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lb2:\n"                    // 5+6=11 → B2
           "      iconst_5\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    LbR:\n"                    // 6+7=13 → R
           "      bipush 6\n"
           "      bipush 7\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lgate:\n"                  // 边载体 junk 检查：紧邻岛头（落空臂
           "      iconst_5\n"              //   positional 直落岛头，钉点回归面）
           "      bipush 9\n"
           "      if_icmplt Ldisp\n"
           "    Ldisp:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 10,\n"
           "          max: 13,\n"
           "          cases: { LB1, LB2, LE, LR },\n"
           "          default: LE\n"
           "      }\n"
           "    LE:\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LB1\n"
           "    LB1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LB2\n"
           "    LB2:\n"
           "      iload 1\n"
           "      iconst_2\n"
           "      imul\n"
           "      iload 0\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LR\n"
           "    LR:\n"
           "      iload 0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具基底：单岛（同 flat，无 scramble）。
std::string flatPlainSource(const std::string& name)
{
    std::string src = flatSource(name);
    const std::string scr =
        "    Lscr:\n"
        "      iconst_1\n"
        "      bipush 99\n"
        "      iadd\n"
        "      istore 2\n"
        "      goto Ldisp\n";
    const ca::usize at = src.find(scr);
    if (at == std::string::npos) return src;
    return src.replace(at, scr.size(), "");
}

/// @brief 拒绝面夹具：span 求值除零，LbR 桥 6/0（IDIV 除数 0，Java 抛
///        ArithmeticException，静态复算不定 → spanUnsolvable 整岛拒）。
std::string divZeroSource(const std::string& name)
{
    std::string src = flatPlainSource(name);
    const std::string needle =
        "    LbR:\n"
        "      bipush 6\n"
        "      bipush 7\n"
        "      iadd\n";
    const ca::usize at = src.find(needle);
    if (at == std::string::npos) return src;
    return src.replace(at, needle.size(),
                       "    LbR:\n"
                       "      bipush 6\n"
                       "      iconst_0\n"
                       "      idiv\n");
}

/// @brief 拒绝面夹具：state 槽越界写，真块 E 以非常量表达式写槽 2
///        （iload 参与 → span 出词汇表 → 槽污染连坐整槽）。
std::string pollutedSource(const std::string& name)
{
    std::string src = flatPlainSource(name);
    const std::string needle =
        "    LE:\n"
        "      iload 0\n";
    const ca::usize at = src.find(needle);
    if (at == std::string::npos) return src;
    src.replace(at, needle.size(),
                "    LE:\n"
                "      iload 0\n"
                "      istore 2\n"            // 越界 state 写（span 不可解）
                "      iload 0\n");
    return src;
}

/// @brief 拒绝面夹具：processor 形桥（Mixed 模式缩样，键在 key processor
///        后静态不可解）+ inline 入口桥，整岛 keyProcessor 保留。
std::string processorSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat (II)I {\n"
           "    code: {\n"
           "    LbP:\n"                    // processor 形桥（INT;ISTORE;ILOAD;INT;INVOKE;ISTORE）
           "      bipush 123\n"
           "      istore 2\n"
           "      iload 2\n"
           "      bipush 45\n"
           "      invokestatic " + name + ".proc (II)I\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    LbE:\n"                    // inline 入口桥：3^15=12 → LE
           "      iconst_3\n"
           "      bipush 15\n"
           "      ixor\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Ldisp:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 10,\n"
           "          max: 12,\n"
           "          cases: { LB1, LB2, LE },\n"
           "          default: LE\n"
           "      }\n"
           "    LE:\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LB1\n"
           "    LB1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LB2\n"
           "    LB2:\n"
           "      iload 1\n"
           "      iconst_2\n"
           "      imul\n"
           "      iload 0\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LB1\n"              // （回边不合法也无妨：夹具不运行）
           "    }\n"
           "  }\n"
           "\n"
           "  .method static proc (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      imul\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：岛间共享块，两岛（不同槽）case 目标同落 LX。
std::string sharedBlockSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat4 (I)I {\n"
           "    code: {\n"
           "    Lb1a:\n"                   // D1 桥：9+91=100 → LX
           "      bipush 9\n"
           "      bipush 91\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    Lb1b:\n"                   // D1 桥：9+92=101 → LB1
           "      bipush 9\n"
           "      bipush 92\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp1\n"
           "    Lb2a:\n"                   // D2 桥：9+191=200 → LX（共享块）
           "      bipush 9\n"
           "      bipush 191\n"
           "      iadd\n"
           "      istore 3\n"
           "      goto Ldisp2\n"
           "    Lb2b:\n"                   // D2 桥：9+192=201 → LB2
           "      bipush 9\n"
           "      bipush 192\n"
           "      iadd\n"
           "      istore 3\n"
           "      goto Ldisp2\n"
           "    Ldisp1:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 100,\n"
           "          max: 101,\n"
           "          cases: { LX, LB1 },\n"
           "          default: LX\n"
           "      }\n"
           "    Ldisp2:\n"
           "      iload 3\n"
           "      tableswitch {\n"
           "          min: 200,\n"
           "          max: 201,\n"
           "          cases: { LX, LB2 },\n"
           "          default: LX\n"
           "      }\n"
           "    LX:\n"
           "      iload 0\n"
           "      ireturn\n"
           "    LB1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      ireturn\n"
           "    LB2:\n"
           "      iload 0\n"
           "      iconst_2\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 反误伤夹具：v3 InvokeDispatcher 分发器方法头（ILOAD key 槽 +
///        tableswitch default 回跳方法头 + case 转发体），岛形判据不得把
///        它当 CFF 岛擦掉（default 非 case 目标 + 零分支引用两道检查）。
std::string dispatcherHeadSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method private static dispA (Ljava/lang/Object;Ljava/lang/Object;I)V {\n"
           "    code: {\n"
           "    Lhead:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 100,\n"
           "          max: 101,\n"
           "          cases: { Lc0, Lc1 },\n"
           "          default: Lhead\n"
           "      }\n"
           "    Lc0:\n"
           "      aload 0\n"
           "      checkcast java/lang/Object\n"
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    Lc1:\n"
           "      aload 0\n"
           "      checkcast java/lang/Object\n"
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method static tgtA (Ljava/lang/Object;)V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：异常区间 start 锚在桥头（被擦块），保护语义不可保持。
///        单岛 + handler 为独立真块（异常 handler 栈面 [exc] 不汇入共享
///        dispatcher，handler 桥若同岛会因 [exc]×[] 合并冲突无法过帧分析，
///        per-frame 第二岛才是生成面正形，见 excSource）。
std::string excStartOnBridgeSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static flat (II)I {\n"
           "    exceptions: { { Lb1, LR, LH, Ljava/lang/Throwable; } },\n"
           "    code: {\n"
           "      iconst_3\n"              // 入口桥：3^15=12 → LE（blocks[0]）
           "      bipush 15\n"
           "      ixor\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lb1:\n"                    // 4+6=10 → LB1（异常 start 锚 → 被擦块）
           "      iconst_4\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Lb2:\n"                    // 5+6=11 → LB2
           "      iconst_5\n"
           "      bipush 6\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    LbR:\n"                    // 6+7=13 → LR
           "      bipush 6\n"
           "      bipush 7\n"
           "      iadd\n"
           "      istore 2\n"
           "      goto Ldisp\n"
           "    Ldisp:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 10,\n"
           "          max: 13,\n"
           "          cases: { LB1, LB2, LE, LR },\n"
           "          default: LE\n"
           "      }\n"
           "    LE:\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto Lb1\n"
           "    LB1:\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto Lb2\n"
           "    LB2:\n"
           "      bipush 100\n"            // 100/y：夹具不运行，只验拒绝面
           "      iload 1\n"
           "      idiv\n"
           "      iload 0\n"
           "      iadd\n"
           "      istore 0\n"
           "      goto LbR\n"
           "    LR:\n"
           "      iload 0\n"
           "      ireturn\n"
           "    LH:\n"                     // handler 真块（handler 边栈 [exc]）
           "      pop\n"
           "      iconst_m1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief JVM 实跑夹具：flat（异常路径版）+ main，反摊平后
///        `java -Xverify:all` 实跑 stdout 应为 "33\n-1"。
std::string jvmSource(const std::string& name)
{
    std::string src = excSource(name);
    // 摘掉类体收尾 "}\n"（方法体闭合保留），拼上 main 后重新收拢。
    while (!src.empty() && (src.back() == '\n' || src.back() == '}')) {
        if (src.back() == '}') {
            src.pop_back();
            break;
        }
        src.pop_back();
    }
    src += "\n"
           "  .method public static main ([Ljava/lang/String;)V {\n"
           "    code: {\n"
           "      iconst_3\n"
           "      iconst_4\n"
           "      invokestatic " + name + ".flat (II)I\n"
           "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
           "      swap\n"
           "      invokevirtual java/io/PrintStream.println (I)V\n"
           "      iconst_3\n"
           "      iconst_0\n"
           "      invokestatic " + name + ".flat (II)I\n"
           "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
           "      swap\n"
           "      invokevirtual java/io/PrintStream.println (I)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
    return src;
}

/// @brief 全部拒绝面夹具打包（每类独立：字节逐位不变的断言载体）。
struct NegativeFixture
{
    std::string name;
    std::string source;
};

std::vector<NegativeFixture> negativeFixtures()
{
    std::vector<NegativeFixture> out;
    out.push_back({"CffPollutedHost", pollutedSource("CffPollutedHost")});
    out.push_back({"CffProcessorHost", processorSource("CffProcessorHost")});
    out.push_back({"CffSharedHost", sharedBlockSource("CffSharedHost")});
    out.push_back({"CffDispHeadHost", dispatcherHeadSource("CffDispHeadHost")});
    out.push_back({"CffExcStartHost", excStartOnBridgeSource("CffExcStartHost")});
    out.push_back({"CffDivZeroHost", divZeroSource("CffDivZeroHost")});
    return out;
}

// ============================================================================
// ==== JVM 实跑辅助（-Xverify:all）===========================================
// ============================================================================

/// @brief 定位 java 可执行：JAVA_HOME/bin/java 优先，PATH 上的 java 兜底；
///        都没有 = 空（调用方 GTEST_SKIP）。
std::string locateJava()
{
    if (const char* home = std::getenv("JAVA_HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path p =
            std::filesystem::path(home) / "bin" / "java.exe";
        if (std::filesystem::exists(p)) return p.string();
        const std::filesystem::path ux = std::filesystem::path(home) / "bin" / "java";
        if (std::filesystem::exists(ux)) return ux.string();
    }
    return "java";
}

/// @brief 反摊平产物落临时目录 + `java -Xverify:all -cp <dir> <main>` 实跑
///        （stdout/stderr 重定向落盘回读，std::system 面，无平台宏）。
/// @return (退出码, 合并输出)；java 不可用返回 (-1, "no java")。
std::pair<int, std::string> runJavaVerify(const std::vector<ca::u8>& bytes,
                                          const std::string&         mainClass)
{
    const std::string java = locateJava();
    if (java == "java" && std::system("java -version >NUL 2>&1") != 0) {
        return {-1, "no java"};
    }

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("cff_unflatten_test_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    const std::string clsUri = mainClass + ".class";
    {
        std::ofstream out(dir / clsUri, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    // cmd /c 对含引号命令行走剥首尾引号老规则，免引号面才是稳定面：
    // 全路径无空格时直接裸拼（java 全路径 / temp 目录带空格 = 环境不支持，
    // 如实报 no java 交由调用方跳过）。
    const std::filesystem::path outPath = dir / "out.txt";
    const std::string dirStr  = dir.string();
    const std::string javaBin = (java.find(' ') == std::string::npos
                                     && dirStr.find(' ') == std::string::npos)
                                    ? java
                                    : std::string();
    if (javaBin.empty()) return {-1, "no java"};

    const std::string cmd = javaBin + " -Xverify:all -cp " + dirStr + " "
                            + mainClass + " > " + outPath.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());
    std::string captured;
    {
        std::ifstream in(outPath, std::ios::binary);
        std::string   line;
        while (std::getline(in, line)) {
            captured += line;
            captured += '\n';
        }
        if (captured.empty()) captured = "<空输出>";
    }
    // 失败诊断面：退出码与命令行随输出一起回传（不落盘、不外泄）。
    if (rc != 0) captured += "[cmd] " + cmd + "\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return {rc, captured};
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(CffScan, FindsSingleIslandWithBridgesAndScramble)
{
    model::ResourceContainer container =
        makeContainer({{"CffFlat", flatSource("CffFlat")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.islandsSolvable, 1u);
    ASSERT_EQ(result.islands.size(), 1u);
    EXPECT_EQ(result.islands.front().uri, "CffFlat.class");
    EXPECT_EQ(result.islands.front().methodIndex, 0u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(CffScan, FindsTwoIslandsOnSharedSlot)
{
    model::ResourceContainer container =
        makeContainer({{"CffMulti", multiSource("CffMulti")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 2u);
    EXPECT_EQ(result.islandsSolvable, 2u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(CffScan, FindsIslandBehindJunkGateCarrier)
{
    model::ResourceContainer container =
        makeContainer({{"CffGate", gateSource("CffGate")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.islandsSolvable, 1u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(CffScan, FindsHandlerBridgeIslandOnExceptionEdge)
{
    // per-frame 第二岛（键窗 [14,15)）与主岛同槽共存；handler 锚桥头可重绑。
    model::ResourceContainer container =
        makeContainer({{"CffExc", excSource("CffExc")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 2u);
    EXPECT_EQ(result.islandsSolvable, 2u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(CffScan, RejectsStateSlotPollution)
{
    model::ResourceContainer container =
        makeContainer({{"CffPollutedHost", pollutedSource("CffPollutedHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.islands.size(), 0u);
    EXPECT_EQ(result.statePolluted, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("槽污染"), std::string::npos);
}

TEST(CffScan, RejectsProcessorModeBridge)
{
    model::ResourceContainer container =
        makeContainer({{"CffProcessorHost", processorSource("CffProcessorHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.islands.size(), 0u);
    EXPECT_EQ(result.keyProcessor, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("processor"), std::string::npos)
        << result.rejects.front().why;
}

TEST(CffScan, RejectsSharedBlocksAcrossIslands)
{
    model::ResourceContainer container =
        makeContainer({{"CffSharedHost", sharedBlockSource("CffSharedHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 2u);
    EXPECT_EQ(result.islands.size(), 0u);
    EXPECT_EQ(result.blocksShared, 2u);   // 两岛连坐
    ASSERT_EQ(result.rejects.size(), 2u);
}

TEST(CffScan, DoesNotMisfireOnInvokeDispatcherHead)
{
    // InvokeDispatcher 方法头（default 回跳方法头 + 零分支引用）不判成 CFF 岛。
    model::ResourceContainer container =
        makeContainer({{"CffDispHeadHost", dispatcherHeadSource("CffDispHeadHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 1u);    // 形状命中候选（iload 2 + tableswitch）
    EXPECT_EQ(result.islandsSolvable, 0u);
    EXPECT_EQ(result.islands.size(), 0u);
    ASSERT_EQ(result.rejects.size(), 1u);
}

TEST(CffScan, RejectsExcRangeStartOnVictimBridge)
{
    model::ResourceContainer container =
        makeContainer({{"CffExcStartHost", excStartOnBridgeSource("CffExcStartHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.islandsSolvable, 0u);
    EXPECT_EQ(result.islands.size(), 0u);
    EXPECT_EQ(result.excBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("异常"), std::string::npos);
}

TEST(CffScan, RejectsSpanEvalDivZero)
{
    // LbR 桥 span 走 IDIV 除数 0：Java 语义是 ArithmeticException，静态复算
    // 无定义 → spanUnsolvable 整岛拒（fail-closed，字节逐位不动见
    // KeepsBytesUntouchedOnRejections）。
    model::ResourceContainer container =
        makeContainer({{"CffDivZeroHost", divZeroSource("CffDivZeroHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.islands.size(), 0u);
    EXPECT_EQ(result.islandsSolvable, 0u);
    EXPECT_EQ(result.spanUnsolvable, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("除零"), std::string::npos)
        << result.rejects.front().why;

    const DirtySet dirty = rewrite(container, result);
    EXPECT_EQ(dirty.islands, 0u);
    EXPECT_TRUE(dirty.uris.empty());
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(CffRewrite, UnflattensSingleIslandAndPrunesScramble)
{
    model::ResourceContainer container =
        makeContainer({{"CffFlat", flatSource("CffFlat")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.islands.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.islands, 1u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.failed, 0u);
    EXPECT_EQ(dirty.scrambles, 1u);
    EXPECT_EQ(dirty.entryRestored, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::string out = disassemble(classBytes(container, "CffFlat.class"));
    // 岛与桥擦除：tableswitch / state span 全数离场。
    EXPECT_EQ(out.find("tableswitch"), std::string::npos) << out;
    EXPECT_EQ(out.find("istore_2"), std::string::npos) << out;
    EXPECT_EQ(out.find("bipush 99"), std::string::npos) << out;
    EXPECT_EQ(out.find("iconst_3"), std::string::npos) << out;
    // 真块链保留（E→B1→B2→R 直线块序）。
    EXPECT_NE(out.find("iload_0"), std::string::npos) << out;
    EXPECT_NE(out.find("ireturn"), std::string::npos) << out;
    EXPECT_NE(out.find("iconst_2"), std::string::npos) << out;
}

TEST(CffRewrite, UnflattensBehindJunkGateCarrier)
{
    model::ResourceContainer container =
        makeContainer({{"CffGate", gateSource("CffGate")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.islands.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.islands, 1u);
    EXPECT_EQ(dirty.failed, 0u);
    EXPECT_EQ(dirty.entryRestored, 1u);

    const std::string out = disassemble(classBytes(container, "CffGate.class"));
    EXPECT_EQ(out.find("tableswitch"), std::string::npos) << out;
    EXPECT_EQ(out.find("istore_2"), std::string::npos) << out;
    // junk 检查原样保留（臂已重指真实块），假臂 ireturn 尾在场。
    EXPECT_NE(out.find("if_icmplt"), std::string::npos) << out;
    EXPECT_NE(out.find("ireturn"), std::string::npos) << out;
}

TEST(CffRewrite, PinsFallthroughArmOfGateAdjacentToDispatcher)
{
    // 无假臂 junk 检查紧邻岛头：if_icmplt 落空臂是位置边（usersOf 看不见），
    // 岛头擦除后必须补 GOTO 钉回 case 目标（key 12 → LE），否则落空臂改落
    // 邻接后继（LB1）。已归类条件 guard 块 → 不拒，钉后正常反摊平。
    model::ResourceContainer container =
        makeContainer({{"CffPinGate", pinGateSource("CffPinGate")}});

    const ScanResult scanned = scan(container, ScanInput{});
    EXPECT_EQ(scanned.dispatchers, 1u);
    ASSERT_EQ(scanned.islands.size(), 1u);
    EXPECT_EQ(scanned.islandsSolvable, 1u);
    EXPECT_EQ(scanned.rejects.size(), 0u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.islands, 1u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.failed, 0u);
    EXPECT_EQ(dirty.entryRestored, 1u);

    const std::string out = disassemble(classBytes(container, "CffPinGate.class"));
    // 岛 + 三桥 + 入口桥 span 全数离场（jasm 反汇编把 bipush 折成 ldc，
    // 桥 span 的 6/7/15 以 ldc 形在场即未擦净）。
    EXPECT_EQ(out.find("tableswitch"), std::string::npos) << out;
    EXPECT_EQ(out.find("istore_2"), std::string::npos) << out;
    EXPECT_EQ(out.find("iconst_3"), std::string::npos) << out;
    EXPECT_EQ(out.find("iconst_4"), std::string::npos) << out;
    EXPECT_EQ(out.find("ixor"), std::string::npos) << out;
    for (const char* k : {"bipush 6", "bipush 7", "bipush 15",
                          "ldc 6", "ldc 7", "ldc 15"}) {
        EXPECT_EQ(out.find(k), std::string::npos) << k << " 在场:\n" << out;
    }
    // junk guard 块体保留（臂重指 LE），真块链在场（bipush 9 经 jasm 折为 ldc 9）。
    EXPECT_NE(out.find("iconst_5"), std::string::npos) << out;
    EXPECT_NE(out.find("ldc 9"), std::string::npos) << out;
    EXPECT_NE(out.find("if_icmplt"), std::string::npos) << out;
    EXPECT_NE(out.find("ireturn"), std::string::npos) << out;
    // goto 计数 = 真块链 3（E→B1→B2→R）+ 落空臂钉 1。缺钉 = 3。
    ca::usize gotos = 0;
    for (ca::usize at = out.find("goto "); at != std::string::npos;
         at = out.find("goto ", at + 1)) {
        ++gotos;
    }
    EXPECT_EQ(gotos, 4u) << out;
}

TEST(CffRewrite, UnflattensTwoIslandsAndRebindsHandler)
{
    model::ResourceContainer container =
        makeContainer({{"CffExc", excSource("CffExc")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.islands.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.islands, 2u);
    EXPECT_EQ(dirty.failed, 0u);
    EXPECT_EQ(dirty.entryRestored, 1u);

    const std::string out = disassemble(classBytes(container, "CffExc.class"));
    EXPECT_EQ(out.find("tableswitch"), std::string::npos) << out;
    EXPECT_EQ(out.find("istore_2"), std::string::npos) << out;
    // 异常表面保留（handler 重绑由 JVM 实跑检查验证；反汇编输出不带 L 前缀）。
    EXPECT_NE(out.find("java/lang/Throwable"), std::string::npos) << out;
    EXPECT_NE(out.find("iconst_m1"), std::string::npos) << out;
}

TEST(CffRewrite, KeepsBytesUntouchedOnRejections)
{
    std::vector<std::vector<ca::u8>> before;
    model::ResourceContainer         container;
    for (const NegativeFixture& f : negativeFixtures()) {
        const std::string         uri   = f.name + ".class";
        const std::vector<ca::u8> bytes = assemble(f.name, f.source);
        before.push_back(bytes);
        container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    }

    const ScanResult scanned = scan(container, ScanInput{});
    EXPECT_EQ(scanned.islandsSolvable, 0u);
    EXPECT_EQ(scanned.statePolluted, 1u);
    EXPECT_EQ(scanned.keyProcessor, 1u);
    EXPECT_EQ(scanned.blocksShared, 2u);
    EXPECT_EQ(scanned.excBlocked, 1u);
    // 1（槽污染）+ 1（processor）+ 2（共享块连坐）+ 1（分发器头反误伤）
    // + 1（异常 start 锚被擦块）+ 1（span 除零）= 7。
    ASSERT_EQ(scanned.rejects.size(), 7u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.islands, 0u);
    EXPECT_TRUE(dirty.uris.empty());

    for (ca::usize i = 0; i < negativeFixtures().size(); ++i) {
        EXPECT_EQ(classBytes(container, negativeFixtures()[i].name + ".class"), before[i])
            << negativeFixtures()[i].name << " 被改写";
    }
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(CffProof, KeepsClassCountReparsesAndResidualZero)
{
    model::ResourceContainer container =
        makeContainer({{"CffExc", excSource("CffExc")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.islands.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.islands, 2u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(CffProof, CountsResidualWhenIslandsUntouched)
{
    // 岛在场但没跑 rewrite：residual 如实报告本可反摊平的岛数。
    model::ResourceContainer container =
        makeContainer({{"CffMulti", multiSource("CffMulti")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.islands.size(), 2u);

    DirtySet dirty;   // 空：没改任何东西
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 2u);
}

// ============================================================================
// ==== JVM 实跑（-Xverify:all）===============================================
// ============================================================================

TEST(CffJvmVerify, UnflattenedMethodRunsAndVerifies)
{
    model::ResourceContainer container =
        makeContainer({{"CffJvmMain", jvmSource("CffJvmMain")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.islands.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.islands, 2u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const auto [rc, output] =
        runJavaVerify(classBytes(container, "CffJvmMain.class"), "CffJvmMain");
    if (rc == -1 && output == "no java") {
        GTEST_SKIP() << "java 不可用（JAVA_HOME/PATH 均缺）";
    }
    EXPECT_EQ(rc, 0) << output;
    EXPECT_NE(output.find("33"), std::string::npos) << output;
    EXPECT_NE(output.find("-1"), std::string::npos) << output;
}

}   // namespace
}   // namespace grunt::cff_unflatten
