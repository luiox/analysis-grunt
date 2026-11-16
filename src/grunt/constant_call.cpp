/// @file        constant_call.cpp
/// @brief       残留 `constant_call` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 判据（三条同时成立才进 sites）
///
/// 粗找 = 全容器扫 INVOKESTATIC 且目标在容器内（不判实参、不判纯度，只保
/// 证不漏）。细找三条：
///
/// 1. **目标可折**：ACC_STATIC 置位；非 ACC_SYNCHRONIZED（监视器获取是可
///    观测行为，删调用就变了）；名字不以 `<` 开头（排除 <init>/<clinit>）；
///    返回类型非 void（无值可折）；在容器内有方法体。
/// 2. **实参定值**：从调用点向前逐条走，每条都是纯常量装载，累计槽数恰好
///    等于描述符参数槽数（long/double 双槽）；中途遇到任何非常量装载
///    （iload、getstatic、dup 等）即拒绝。ldc 经会话常量池解析：数值常量
///    接受，String/Class/MethodHandle 等一律拒绝（不猜内容）。
/// 3. **求值成功**：mjbase 的 asm_evaluator 在常量实参下跑目标方法。求值
///    子集只含数值运算、栈整形、分支与返回，不含字段访问、数组访问、方
///    法调用、对象分配、显式抛出，因此跑出值等价于这条执行路径无堆
///    副作用。
///
/// ## 类初始化检查与同类豁免
///
/// 被调类的 <clinit> 必须中性（scan_tools::classInitNeutral 双面口径：缺失
/// 或空壳；或纯常量初值面——只写本类字段的确定值，初始化时机不可观察）：
/// invokestatic 是类的主动使用，删掉调用会改变类初始化时机。同类调用豁免
/// （JVMS 5.5）：本类方法开始执行时类已在初始化中/已完成，同类 INVOKESTATIC
/// 不触发初始化，删调用不移时机。豁免只作用于初始化时机这一个检查，纯性、
/// 常量实参、求值三条判据对同类站点照收不误。
///
/// ## 求值环境接管：java/lang/Math.max(II)I
///
/// IndyRestore 还原 `__grt$indy$` 转发体内的 indy 后，CFF inline 桥的
/// MaxConst 站点（grunt3 `StateOp.MaxConst.emitTo` 逐字）从转发调用变
/// 成转发体内 invokestatic java/lang/Math.max(II)I，求值转发体必然踩到
/// 这条 JDK 调用；而 jsim 核心/num 包没有 JDK 数学面，缺接管时整条面在
/// EvalBadState 拒。求值会话按签名注册 Math.max 接管：弹双参、全序取大；
/// unknown 按未知口径传播（结果落 ValueKindUnsupported），不编值。max 是
/// 全序纯函数，双常量实参下值唯一确定，接管不引入猜测面。
///
/// ## 为什么求值放在 scan 里
///
/// 三段的状态需求本来就不对称，只有 scan 需要跨站点的昂贵缓存。求值是判
/// 定的一部分（这个调用是不是常量），不是改写的一部分；放进 scan 才能
/// 让 rewrite 只消费结果、不回头找站点。
///
/// ## REWRITE（每站六步，任一步失败整类放弃）
///
/// 按 调用类 → 调用方法 → 站点 分组。改写前先过一道**往返自检**：在未改
/// 动的独立会话上把方法 promote → lower 走一遍，走不完的整方法跳过
/// （skippedByRoundTrip）。理由：lower 不是 promote 的恒等逆（编码器对少数
/// 布局/属性组合明确拒绝），这类方法与改写无关，但一旦被改写就会把整个类
/// 拖下水，表现为改写有 bug；先探一遍，失败面就只剩真正的改写缺陷。
///
/// 每站六步：按 callSrcPc 在重新 promote 的 DOM 里找回调用指令（InsnId 是
/// 会话内句柄不能跨会话带出，所以站点用原始 pc 定位）；校验成员引用与
/// ScanResult 一致；用与 scan 相同的规则重新收集实参并逐值比对（防 scan
/// 与 rewrite 之间 DOM 语义漂移，不一致放弃该站点）；造常量装载
/// （iconst/bipush/sipush 优先，放不下才落池，池索引一律经
/// CodeEditor::loadConst 选形态，LDC 操作数域只有 u1，超界编码器直接
/// Unencodable）；把首个实参装载**原地改写**成常量装载，eraseBatch 擦掉
/// 其余实参装载 + 调用；失败把首条写回原样，DOM 保持原样。
///
/// ### 为什么是原地改写首条而不是插一条 + 删首条
///
/// 不是风格选择，是唯一能走通的路：CodeDom::insertAt 把新指令的 srcPc 硬
/// 清成 kNoPc（合成指令没有旧位置，DOM 不变量），而 lowerMethod 的属性重
/// 定位全靠 pcmap，LineNumberTable 每行、LocalVariableTable 起止、
/// StackMapTable 每帧都要精确命中一个旧 pc，命中不了就 RelocationBadTarget
/// 整体失败。删掉首条会让锚在区间起点的行全部失去落点，整个方法连同类一
/// 起作废（注入的实参装载继承被替换指令的行号/帧位，样本里几乎必然）。
/// 原地改写保住首条的 srcPc：锚在它上面的行照常重定位，锚在它上面的标签
/// 不必迁移。区间其余指令的旧 pc 随之消失，因此额外要求这些指令上没有标
/// 签，有标签说明分支/switch/异常范围从区间中段进入，删掉会让目标悬垂
/// （LabelBlocked 整站放弃）；首条不受此限。
///
/// 栈形不变：原序列 n 条装载（合计 s 槽）+ invokestatic（弹 s 压 r，
/// r ∈ {1,2}），新序列 1 条装载（压 r 槽），调用前后栈深相同，
/// StackMapTable 按新布局重算即可，maxStack 沿用原值（新峰值 base+1 不超
/// 原峰值 base+s）。
///
/// ## PROOF
///
/// 两道检查：类数不增；每个脏类重新开会话可解析（抓写出的字节自己都读不
/// 回来这类错误）。帧与校验器层面的验收在语料回归里做
/// （test/tools/VerifyJar.java 逐类过校验器，需要 JDK，不进单元测试）。
///
/// ## 已知边界
///
/// 不做跨方法常量传播：目标读字段或调别的纯方法时求值器 Unsupported，站
/// 点被拒。不做 <clinit> 常量传播：getstatic + iaload 的字段池化数字不在
/// 面内。引用结果只折解密器族：通用站点的返回 String/对象不折（引用身份
/// 不可静态构造）；解密器族是例外——产物 String 由封闭求值从堆提取回
/// 码元后 intern 进池，身份无关紧要，值才是语义（形态判据见解密器族段）。
/// processor 键解锁链是静态天花板：CFF 桥 key processor 形
/// （五条窗口 [INT; ISTORE; ILOAD; INT; INVOKESTATIC proc]）的 proc 恢复目
/// 标全部落在 native 摘除的 action 壳上（算术已随 native pipeline 出
/// jar），实参又是 ILOAD 夹心形非紧邻常量，两道静态墙叠加，这部分永远
/// 不可静态折，属 CffUnflatten key-processor 桶的永久余量；可折的只有
/// Math.max 转发面。
#include "grunt/constant_call.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/indy.hpp"
#include "jsim/num.hpp"
#include "jsim/reflect.hpp"
#include "jsim/str.hpp"

namespace grunt::constant_call {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

/// 扫描面 fail-closed 包裹：单个类里的任何异常只丢该类的候选，不放大成
/// 整条管线失败。本残留是全局 pass，异常会一路穿到 mjbase 管线的
/// `catch (...)`，CLI 直接 exit 3（unknown exception整包拒绝），
/// 对齐仓内保守口径（rewrite_toolbox::mutateMethodWithFrames 的两级收口）：
/// std::exception 带 what()，其余兜底；该类跳过并打 stderr 日志。
template <class Fn>
bool scanGuard(std::string_view stage, std::string_view uri, Fn&& fn)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[grunt.ConstantCall] %.*s skip class %.*s: %s\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[grunt.ConstantCall] %.*s skip class %.*s: unknown exception\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data());
    }
    return false;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// 返回类别（'V' 表示 void）。
char returnChar(std::string_view desc)
{
    const ca::usize rp = desc.find(')');
    if (rp == std::string_view::npos || rp + 1 >= desc.size()) return 'V';
    return desc[rp + 1];
}

// ============================================================================
// ==== 解密器族（string decryptor family）===================================
// ============================================================================

// Grunt 家族把字符串池的密文/密钥/种子固化在每个池填充点的实参里：
//
//     getstatic a ; iconst_0                    ← aastore 的目标与下标（非实参）
//     ldc_w "密文"                              ← 栈最深
//     invokevirtual String.toCharArray:()[C
//     ldc2_w <key:long>
//     ldc <seed:int>                            ← 离调用最近
//     invokestatic a:([CJI)Ljava/lang/String;
//     aastore
//
// 目标方法是同类 private static 的 junk 状态机（lookupswitch 分派 +
// char[] 逐元异或 + new String(char[])），对常量实参返回值唯一确定。
// 回收集序（NOP 跳过）固定为 seed·key·toCharArray·cipher——cipher 在栈
// 最深、seed 最近。族判据三面：
//
//   1. **形态**：回收集 4 条正好是 I32 装载·I64 装载·String.toCharArray·
//      ldc String（decodeValueLoad 校验），密文经 MUTF-8 解回码元；
//   2. **同类**（JVMS 5.5）：解密器与调用点同 owner——同类 INVOKESTATIC
//      不触发类初始化，删调用不移时机；跨类调用一律不折（Grunt 家族池
//      填充与解密器恒同类，此限制不损失面）；
//   3. **求值**：jsim 执行器带堆实参（char[] 从码元落 heap）实跑解密器体，
//      返回的 String 从堆提取回码元。求值成功 = 这条执行路径可静态重放。
//
// 桥面注记：解密器体是 junk 状态机，死陷阱走廊里必有一颗 athrow，libjsim
// v1 桥对 athrow 整方法拒——jsim_executor 的桥前可达性剥离（根 = 入口 +
// 异常表 handler，不可达指令擦零）让真实解密器可桥（b/B 族实证，明文
// "PASS" 363 步收敛）。剥离只作用于 executor 临时会话，容器字节不动。

/// 形态匹配产出（scan 证据 + rewrite 复核共用）。
struct DecryptorShape
{
    std::vector<ir::InsnId> producers;   ///< 收集序：seed·key·toCharArray·cipher
    ca::i32                 seed = 0;
    ca::i64                 key  = 0;
    std::vector<ca::u16>    cipherUnits;
};

/// 解密器形态匹配：从 callPos 向前 NOP 跳过回收集 4 条并逐条判据。
bool matchDecryptorShape(ir::ClassSession& s, ir::CodeDom& dom,
                         const std::vector<ir::InsnId>& order, ca::usize callPos,
                         DecryptorShape& out)
{
    std::vector<const ir::Insn*> ins;
    std::vector<ir::InsnId>      ids;
    for (ca::usize q = callPos; q-- > 0 && ins.size() < 4;) {
        const ir::Insn* cur = dom.peek(order[q]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        ins.push_back(cur);
        ids.push_back(order[q]);
    }
    if (ins.size() < 4) return false;

    // ins[2] 必须是 String.toCharArray ()[C（INVOKEVIRTUAL）。
    if (ins[2]->op != Op::INVOKEVIRTUAL) return false;
    ir::ClassSession::MemberSem cm;
    if (!s.memberOf(*ins[2], cm)) return false;
    if (cm.owner != "java/lang/String" || cm.name != "toCharArray" || cm.desc != "()[C") {
        return false;
    }

    // 实参装载判据：seed(I32)·key(I64)·cipher(Str)。
    const_args::ValueLoad seedV;
    const_args::ValueLoad keyV;
    const_args::ValueLoad cipherV;
    if (!const_args::decodeValueLoad(s, *ins[0], seedV)
        || seedV.kind != const_args::ValueLoad::Kind::I32) {
        return false;
    }
    if (!const_args::decodeValueLoad(s, *ins[1], keyV)
        || keyV.kind != const_args::ValueLoad::Kind::I64) {
        return false;
    }
    if (!const_args::decodeValueLoad(s, *ins[3], cipherV)
        || cipherV.kind != const_args::ValueLoad::Kind::Str) {
        return false;
    }
    std::vector<ca::u16> units;
    if (!scan_tools::mutf8ToUnits(cipherV.str, units)) return false;

    out.producers   = std::move(ids);
    out.seed        = seedV.i32();
    out.key         = keyV.i64();
    out.cipherUnits = std::move(units);
    return true;
}

/// 容器方法索引构建：scan_tools::buildMethodIndex 收口，本 pass 只钉两件
/// 口径，重名类撞键取先到（emplace 原形 = KeepFirst）、stderr 前缀走本
/// pass 的 scanGuard。
void buildIndex(const mjbase::model::ResourceContainer& container,
                scan_tools::MethodIndex& index)
{
    auto guard = [](std::string_view stage, std::string_view uri, auto&& fn) {
        return scanGuard(stage, uri, fn);
    };
    scan_tools::buildMethodIndex(container, index, scan_tools::IndexDup::KeepFirst, guard);
}

/// parseUnsignedLong 纯函数核（(字符串, 进制) 定值，全序无副作用）：radix
/// 越族（<2 / >36）拒；数字位超进制拒；累进按 u64 回绕口径折返（Grunt 键
/// 面构造恒在 u64 域内，回绕分支只是防御面）。jsim callMock 与容器外内建
/// 站点求值共用同一语义源，两边不许各写一份。
/// @return false = radix/数字位非法（JDK 侧 NumberFormatException 语义）
bool parseULPure(std::string_view text, ca::i32 radix, ca::i64& out)
{
    if (radix < 2 || radix > 36) return false;
    ca::u64 acc = 0;
    for (const char c : text) {
        ca::u32 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<ca::u32>(c - '0');
        } else if (c >= 'a' && c <= 'z') {
            digit = static_cast<ca::u32>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = static_cast<ca::u32>(c - 'A') + 10;
        } else {
            return false;
        }
        if (digit >= static_cast<ca::u32>(radix)) return false;
        acc = acc * static_cast<ca::u64>(radix) + static_cast<ca::u64>(digit);
    }
    out = static_cast<ca::i64>(acc);
    return true;
}

/// 容器外 JDK 全序纯内建白名单（对外站点的唯一入表面）：每个成员必须有
/// (a) 纯函数核（定值、无副作用、全序——parseULPure 一型）(b) 专用实参收
/// 集器（collectBuiltinArgsParseUl 一型）(c) jsim callMock 三件套在位，
/// 扩面按此登记；bootstrap 类初始化时机由 JVM 托管，删调用无可观测变化。
/// 成员登记同步：evaluateSites 头部的 callMock 注册与本表一成员一注册。
struct ExternalBuiltin
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};
constexpr ExternalBuiltin kExternalPureBuiltins[] = {
    {"java/lang/Long", "parseUnsignedLong", "(Ljava/lang/String;I)J"},
};

/// 白名单查询走 keyOf 本尊（'\n' 复合键；手拼 '.' 键会静默 miss，本轮实测
/// 踩坑留痕）。
bool isExternalPureBuiltin(std::string_view owner, std::string_view name, std::string_view desc)
{
    const std::string key = scan_tools::keyOf(owner, name, desc);
    for (const ExternalBuiltin& b : kExternalPureBuiltins) {
        if (key == scan_tools::keyOf(b.owner, b.name, b.desc)) return true;
    }
    return false;
}

/// 求值段：装好扩展包的 jsim 执行器逐站点跑目标方法；通过者写回 `value`，
/// 失败者转成拒绝记录。类初始化检查（scan_tools::classInitNeutral）先判。
void evaluateSites(const mjbase::model::ResourceContainer& container,
                   std::vector<Site>& sites, std::vector<RejectedSite>& rejects)
{
    std::unordered_map<std::string, std::vector<ca::usize>> byOwner;
    for (ca::usize i = 0; i < sites.size(); ++i) byOwner[sites[i].targetOwner].push_back(i);

    std::vector<ca::u8> ok(sites.size(), 0);   // 0 待判 / 1 通过 / 2 拒绝

    SimExecutor exec(container);
    jsim::str::install(exec.session());
    // `new String(char[])` 接管：解密器族的产出提取链最后一环（解密器体
    // 尾部 new String(char[]) → 堆 String → 明文码元）。对通用站点无影响
    // （纯算术目标触不到构造器）。
    jsim::str::installStringAlloc(exec.session());
    jsim::num::install(exec.session());
    // 折叠不需要注解/类元数据：目标按判据是纯算术，反射元数据无人读。
    jsim::reflect::install(exec.session(), [](const std::string&)
                               -> const jsim::reflect::ClassMeta* { return nullptr; });
    jsim::indy::install(exec.session());

    // Math.max (II)I 接管（本文件内最小扩面，申报理由： IndyRestore 还原
    // `__grt$indy$` 转发体内的 indy 后，CFF inline 桥的 MaxConst 站点从
    // 转发调用还原成转发体内 invokestatic java/lang/Math.max(II)I；
    // 求值转发体时必然踩到这条 JDK 调用，jsim 核心/num 包没有 JDK 数学面，
    // 缺这个接管整条面在 EvalBadState 拒，processor 键解锁链断在最后一环。
    // Math.max 是全序纯函数：双常量实参下值唯一确定；unknown 按未知口径
    // 传播（折叠面 ValueKindUnsupported 拒），不编值。）
    exec.session().callMocks["java/lang/Math.max(II)I"] =
        [](jsim::Frame& frame) -> jsim::Result<jsim::SimValue, jsim::Bail> {
            jsim::Result<jsim::SimValue, jsim::Bail> b = frame.popChecked();
            if (!b.isOk()) return b;
            jsim::Result<jsim::SimValue, jsim::Bail> a = frame.popChecked();
            if (!a.isOk()) return a;
            if (!a.value().known || !b.value().known) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                    jsim::SimValue::unknownOf(jsim::ValType::I32));
            }
            const ca::i32 va = static_cast<ca::i32>(a.value().intBits);
            const ca::i32 vb = static_cast<ca::i32>(b.value().intBits);
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                jsim::SimValue::knownI32(va > vb ? va : vb));
        };

    // Long.parseUnsignedLong (Ljava/lang/String;I)J 接管（t 族实证：解密器体
    // 内部经 parseUL 反推密钥分量，缺接管整条面在 Bail 拒）。全序纯函数，
    // 语义核在 parseULPure（与容器外内建站点求值共用）。unknown 按未知口径
    // 传播；radix 越族 / 数字位非法按 NumberFormatException 语义 Bail。
    // mock 键从 kExternalPureBuiltins 派生（一成员一注册，结构性同步）。
    const std::string parseUlMockKey = std::string(kExternalPureBuiltins[0].owner) + "."
                                       + std::string(kExternalPureBuiltins[0].name)
                                       + std::string(kExternalPureBuiltins[0].desc);
    exec.session().callMocks[parseUlMockKey] =
        [&exec](jsim::Frame& frame) -> jsim::Result<jsim::SimValue, jsim::Bail> {
            jsim::Result<jsim::SimValue, jsim::Bail> radixV = frame.popChecked();
            if (!radixV.isOk()) return radixV;
            jsim::Result<jsim::SimValue, jsim::Bail> strV = frame.popChecked();
            if (!strV.isOk()) return strV;
            if (!radixV.value().known || !strV.value().known) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                    jsim::SimValue::unknownOf(jsim::ValType::I64));
            }
            std::string text;
            if (strV.value().ref.kind == jsim::RefPayload::Kind::Str) {
                text = strV.value().ref.str;
            } else if (strV.value().ref.kind == jsim::RefPayload::Kind::Obj) {
                const auto hit = exec.session().heap.find(strV.value().ref.objId);
                if (hit == exec.session().heap.end()
                    || hit->second.className != "java/lang/String") {
                    return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                        {jsim::Bail::Reason::BadOperand, frame.pc(), "parseUL 接收者非 String"});
                }
                text = hit->second.data;
            } else {
                return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                    {jsim::Bail::Reason::BadOperand, frame.pc(), "parseUL 接收者为 null"});
            }
            ca::i64 v = 0;
            if (!parseULPure(text, static_cast<ca::i32>(radixV.value().intBits), v)) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                    {jsim::Bail::Reason::BadOperand, frame.pc(), "parseUL 数字位/进制非法"});
            }
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(jsim::SimValue::knownI64(v));
        };

    // 容器外内建站点：目标无容器方法体，不经 jsim 方法执行，直接走白名单
    // 内建的纯函数核。parseUL 是池读点下标键面的解锁器（b/u 族实证：读点
    // 下标 = (int)(parseUL(str, radix) ^ const)，此站不折则 NF 铺不开算术、
    // PAF 终态表查不到下标，池读点永远残留）。
    for (ca::usize si = 0; si < sites.size(); ++si) {
        Site& site = sites[si];
        if (!site.isExternalBuiltin) continue;
        const bool argShapeOk = site.args.size() == 2 && site.args[0].known
                                && site.args[0].type == jsim::ValType::Ref
                                && site.args[0].ref.kind == jsim::RefPayload::Kind::Str
                                && site.args[1].known
                                && site.args[1].type == jsim::ValType::I32;
        ca::i64 v = 0;
        if (!argShapeOk
            || !parseULPure(site.args[0].ref.str,
                            static_cast<ca::i32>(site.args[1].intBits), v)) {
            rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                           site.callSrcPc, RejectReason::EvalBadState});
            ok[si] = 2;
            continue;
        }
        site.value = jsim::SimValue::knownI64(v);
        ok[si]     = 1;
    }

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("evaluate", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const std::string_view self = session->backing().thisName();

            const auto group = byOwner.find(std::string(self));
            if (group == byOwner.end()) return;

            if (!scan_tools::classInitNeutral(*session)) {
                // 类初始化检查（双面中性口径见 scan_toolbox）：非中性时跨类
                // 调用删掉会改变被调类的初始化时机，照旧整组拒绝。同类调用
                // （boar proc 转发形态：NativePreProcessor 的桥与转发同类）
                // 豁免，JVMS 5.5：本类方法开始执行时类已经在初始化中/已完成，
                // 同类 INVOKESTATIC 不触发初始化，删调用不移时机；豁免站点的
                // 纯性由下面的封闭环境求值独立证明（boar 取证：全部 3262 个
                // 可折转发站点都落在非中性 clinit 类里，检查不豁免则整条
                // processor 键解锁链空转）。
                bool anyKept = false;
                for (const ca::usize si : group->second) {
                    const Site& site = sites[si];
                    if (site.targetOwner == site.callerOwner) {
                        anyKept = true;
                        continue;
                    }
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc,
                                                   RejectReason::ClassInitPresent});
                    ok[si] = 2;
                }
                if (!anyKept) return;
            }

            for (const ca::usize si : group->second) {
                Site& site = sites[si];

                if (ok[si] == 2) continue;   // 检查组拒绝已计数

                if (site.isDecryptor) {
                    // 解密器求值：密文码元落堆成 char[]，jsim 实跑解密器体，
                    // 返回 String 从堆提取回码元。installStringAlloc 的
                    // `new String(char[])` 接管是这条提取链的最后一环。
                    const jsim::u64 arrId = exec.session().nextAllocId++;
                    jsim::HeapObj         arr;
                    arr.className = "[C";
                    arr.elems.reserve(site.cipherUnits.size());
                    for (const ca::u16 u : site.cipherUnits) {
                        arr.elems.push_back(static_cast<jsim::i64>(u));
                    }
                    exec.session().heap[arrId] = std::move(arr);
                    const std::vector<jsim::SimValue> dargs{jsim::SimValue::knownObj(arrId),
                                                            jsim::SimValue::knownI64(site.key),
                                                            jsim::SimValue::knownI32(site.seed)};
                    const SimOutcome out =
                        exec.invoke(site.targetOwner, site.targetName + site.targetDesc, dargs);
                    if (!out.ok() && std::getenv("GRUNT_DEBUG_FOLD") != nullptr) {
                        std::fprintf(stderr,
                                     "[dec-eval-fail] %s.%s pc=%u kind=%d detail=%s\n",
                                     site.callerOwner.c_str(), site.callerMethod.c_str(),
                                     site.callSrcPc, static_cast<int>(out.kind),
                                     out.detail.c_str());
                    }
                    if (!out.ok()) {
                        rejects.push_back(RejectedSite{
                            site.callerOwner, site.callerMethod, site.callSrcPc,
                            out.kind == SimOutcome::Kind::LimitHit ? RejectReason::EvalStepLimit
                                                                   : RejectReason::EvalBadState});
                        ok[si] = 2;
                        continue;
                    }
                    bool plainOk = false;
                    if (out.value.known && out.value.type == jsim::ValType::Ref
                        && out.value.ref.kind == jsim::RefPayload::Kind::Obj) {
                        const auto hit = exec.session().heap.find(out.value.ref.objId);
                        if (hit != exec.session().heap.end()
                            && hit->second.className == "java/lang/String") {
                            std::vector<ca::u16> plain;
                            std::string          plainText;
                            if (scan_tools::mutf8ToUnits(hit->second.data, plain)
                                && rewrite_tools::u16ToUtf8Record(plain, plainText)) {
                                site.plainUnits = std::move(plain);
                                site.plaintext  = std::move(plainText);
                                plainOk         = true;
                            }
                        }
                    }
                    if (!plainOk) {
                        rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                       site.callSrcPc,
                                                       RejectReason::ValueKindUnsupported});
                        ok[si] = 2;
                        continue;
                    }
                    ok[si] = 1;
                    continue;
                }

                if (!ir::hasCode(*session, site.targetMethodIndex)) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetNoCode});
                    ok[si] = 2;
                    continue;
                }

                const SimOutcome out =
                    exec.invoke(site.targetOwner, site.targetName + site.targetDesc, site.args);
                if (out.kind == SimOutcome::Kind::Void) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetVoid});
                    ok[si] = 2;
                    continue;
                }
                if (!out.ok()) {
                    rejects.push_back(RejectedSite{
                        site.callerOwner, site.callerMethod, site.callSrcPc,
                        out.kind == SimOutcome::Kind::LimitHit ? RejectReason::EvalStepLimit
                                                               : RejectReason::EvalBadState});
                    ok[si] = 2;
                    continue;
                }
                if (!out.value.known || out.value.type == jsim::ValType::Ref) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc,
                                                   RejectReason::ValueKindUnsupported});
                    ok[si] = 2;
                    continue;
                }
                site.value = out.value;
                ok[si]     = 1;
            }
        });
    });

    // 没被任何目标类覆盖的站点（目标类解析失败等）统一拒绝。
    std::vector<Site> kept;
    kept.reserve(sites.size());
    for (ca::usize i = 0; i < sites.size(); ++i) {
        if (ok[i] == 1) {
            kept.push_back(std::move(sites[i]));
        } else if (ok[i] == 0) {
            rejects.push_back(RejectedSite{sites[i].callerOwner, sites[i].callerMethod,
                                           sites[i].callSrcPc, RejectReason::TargetNoCode});
        }
    }
    sites = std::move(kept);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// 改写失败原因（trace 用）。
enum class RwResult : ca::u8 {
    Ok = 0,
    CallNotFound,
    MemberMismatch,
    ArgsMismatch,
    LoadFail,
    InsertFail,
    LabelBlocked,
    EraseFail,
};

// ---- 诊断：谁在指将被擦除的 pc（GRUNT_DEBUG_PCREFS）---------------------
//
// 擦掉指令后这些旧 pc 不在 lower 的 pcmap 里，fail-closed 重定位会直接拒
// （RelocationBadTarget）。这个转储把哪类属性、多少行摆出来，用来判
// 该由改写侧一并作废，还是该整站点放弃。

bool pcIn(const std::vector<ca::u32>& dead, ca::u32 pc)
{
    return std::find(dead.begin(), dead.end(), pc) != dead.end();
}

void dumpPcRefs(ir::ClassSession& session, ca::usize methodIndex,
                const std::vector<ca::u32>& dead)
{
    if (std::getenv("GRUNT_DEBUG_PCREFS") == nullptr || dead.empty()) return;

    jbc::core::ClassFile& cf = session.backing();
    const ir::ClassUnit&  u  = session.unit();
    if (methodIndex >= u.methods.size()) return;
    const ca::usize backing = u.methods[methodIndex].backingIndex;
    if (backing >= cf.methods.size()) return;

    auto&          attrs = cf.methods[backing].attrs;
    ca::usize      codeSlot = kNoIndex;
    for (ca::usize i = 0; i < attrs.size(); ++i) {
        if (cf.utf8(attrs[i].nameCp) == "Code") {
            codeSlot = i;
            break;
        }
    }
    if (codeSlot == kNoIndex) return;
    // Code 槽与嵌套属性统一先过注解族深度预检（mjbase #158）：Code 内可挂
    // 注解族（TypeAnnotations），libjbc 注解码码互递归无界，敌意深嵌输入
    // 会在 decodeAttr 内栈溢出；预检拒绝按解不开原样保真跳过，诊断面
    // 少打一行，进程不被打爆。
    if (!mjbase::util::attrSafeToDecode(cf, attrs[codeSlot])) return;
    auto d = cf.decodeAttr(attrs[codeSlot]);
    if (d.is_err()) return;
    auto& code = static_cast<jbc::core::CodeAttr&>(*d.unwrap());

    std::fprintf(stderr, "[pcrefs] dead=");
    for (const ca::u32 pc : dead) std::fprintf(stderr, " %u", pc);
    std::fprintf(stderr, " | codeLen=%zu nested=%zu\n", code.code.size(), code.attrs.size());

    for (jbc::core::AttrSlot& nested : code.attrs) {
        if (!mjbase::util::attrSafeToDecode(cf, nested)) continue;
        auto nd = cf.decodeAttr(nested);
        if (nd.is_err()) continue;
        jbc::core::AttrBody* nb = nd.unwrap();
        switch (nb->kind()) {
        case jbc::core::AttrKind::LineNumberTable: {
            auto& lnt = static_cast<jbc::core::LineNumberTableAttr&>(*nb);
            ca::usize hit = 0;
            for (const auto& row : lnt.rows) hit += pcIn(dead, row.startPc) ? 1 : 0;
            std::fprintf(stderr, "[pcrefs]   LNT rows=%zu dead=%zu\n", lnt.rows.size(), hit);
            break;
        }
        case jbc::core::AttrKind::LocalVariableTable: {
            auto& lvt = static_cast<jbc::core::LocalVariableTableAttr&>(*nb);
            ca::usize hitStart = 0, hitEnd = 0;
            for (const auto& row : lvt.rows) {
                hitStart += pcIn(dead, row.startPc) ? 1 : 0;
                hitEnd += pcIn(dead, row.startPc + row.length) ? 1 : 0;
            }
            std::fprintf(stderr, "[pcrefs]   LVT rows=%zu deadStart=%zu deadEnd=%zu\n",
                         lvt.rows.size(), hitStart, hitEnd);
            break;
        }
        case jbc::core::AttrKind::StackMapTable: {
            auto&     smt  = static_cast<jbc::core::StackMapTableAttr&>(*nb);
            ca::usize hit  = 0;
            ca::u32   prev = 0;   // 首帧的前一帧 pc 约定为 -1，故 prev = -1 的等价起点
            for (const auto& fr : smt.frames) {
                const ca::u32 abs = jbc::core::smtAbsPc(prev, fr.offset);
                hit += pcIn(dead, abs) ? 1 : 0;
                prev = abs;
            }
            std::fprintf(stderr, "[pcrefs]   SMT frames=%zu dead=%zu\n", smt.frames.size(), hit);
            break;
        }
        default:
            std::fprintf(stderr, "[pcrefs]   kind=%d\n", static_cast<int>(nb->kind()));
            break;
        }
    }
}

/// 解密器族站点改写：锚 = 密文 ldc（栈最深 producer）原地改成明文 ldc，
/// 其余 producer（seed·key·toCharArray）+ 调用指令擦除。
///
/// 与通用站点的锚位相反（通用 = 首条实参装载原地改写）：解密器栈形是
/// `ldc cipher(1) · toCharArray(弹压持平) · ldc2_w key(2) · ldc seed(1) ·
/// call(弹 5 压 1)`，cipher 是唯一能原地保留 srcPc 的 producer——它也是
/// 栈最深的，锚在它上面的行/标签随原地改写全部保住。新序列 `ldc 明文`
/// 压 1 槽，调用前后栈深不变，maxStack 沿用（新峰值 base+1 ≤ 原 base+4）。
///
/// 六步：callSrcPc 重定位 → 成员一致性 → 形态 re-match（rewrite 会话重新
/// 收集，防 scan 与 rewrite 间 DOM 语义漂移）→ seed/key/cipher 逐值复核 →
/// internPlainString + loadConst 选形 → 标签守门（victims 上无标签）+
/// prunePcAttrs（擦除路径 LNT/LVT 配套，行面调试无损）→ anchor 原地写 +
/// eraseBatch（失败回滚 anchor）。
RwResult rewriteDecryptorSite(ir::ClassSession& session, ir::CodeUnit& cu, const Site& site)
{
    ir::CodeDom&                   dom   = cu.dom;
    const std::vector<ir::InsnId>& order = dom.order();

    ca::usize callPos = kNoIndex;
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = dom.peek(order[i]);
        if (in != nullptr && in->op == Op::INVOKESTATIC && in->srcPc == site.callSrcPc) {
            callPos = i;
            break;
        }
    }
    if (callPos == kNoIndex) return RwResult::CallNotFound;

    const ir::Insn* call = dom.peek(order[callPos]);
    ir::ClassSession::MemberSem enc;
    if (!session.memberOf(*call, enc)) return RwResult::MemberMismatch;
    if (enc.owner != site.targetOwner || enc.name != site.targetName
        || enc.desc != site.targetDesc) {
        return RwResult::MemberMismatch;
    }

    DecryptorShape shape;
    if (!matchDecryptorShape(session, dom, order, callPos, shape)) {
        return RwResult::ArgsMismatch;
    }
    if (shape.seed != site.seed || shape.key != site.key
        || shape.cipherUnits != site.cipherUnits) {
        return RwResult::ArgsMismatch;
    }
    if (shape.producers.size() != 4) return RwResult::ArgsMismatch;
    // 空明文照常落折（internPlainString 内部处理 findUtf8 占位槽陷阱）。

    const ca::u32 strIdx = rewrite_tools::internPlainString(session.backing().cp,
                                                            site.plainUnits);
    if (strIdx == 0 || strIdx > 0xFFFF) return RwResult::LoadFail;
    ir::Insn load = ir::CodeEditor::loadConst(strIdx);
    load.srcPc = jbc::ir::kNoPc;   ///< 原地写时被 keepPc 覆盖

    // order 是 DOM 的活视图：先把句柄取出来。anchor = cipher（收集序最末，
    // 栈最深）；victims = seed·key·toCharArray + 调用。
    const ir::InsnId callId   = order[callPos];
    const ir::InsnId anchorId = shape.producers.back();
    std::vector<ir::InsnId> victims(shape.producers.begin(), shape.producers.end() - 1);
    victims.push_back(callId);

    for (const ir::InsnId id : victims) {
        if (!dom.labelsAt(id).empty()) return RwResult::LabelBlocked;
    }

    // 擦除路径配套：victim pc 从 pcmap 蒸发，锚行先剪（调试面删行无损）。
    std::vector<ca::u32> victimPcs;
    victimPcs.reserve(victims.size());
    for (const ir::InsnId id : victims) {
        const ir::Insn* v = dom.peek(id);
        if (v != nullptr && v->srcPc != jbc::ir::kNoPc) victimPcs.push_back(v->srcPc);
    }
    const ir::ClassUnit& unit = session.unit();
    if (site.callerMethodIndex >= unit.methods.size()) return RwResult::InsertFail;
    const ca::usize backingIdx = unit.methods[site.callerMethodIndex].backingIndex;
    if (backingIdx >= session.backing().methods.size()) return RwResult::InsertFail;
    if (!rewrite_tools::prunePcAttrs(session.backing(),
                                     session.backing().methods[backingIdx], victimPcs)) {
        return RwResult::EraseFail;
    }

    auto slotR = dom.get(anchorId);
    if (slotR.is_err()) return RwResult::InsertFail;
    ir::Insn*      anchor   = slotR.unwrap();
    const ir::Insn original = *anchor;
    const ca::u32  keepPc   = anchor->srcPc;
    *anchor                 = load;
    anchor->srcPc           = keepPc;

    if (dom.eraseBatch(victims).is_err()) {
        *anchor = original;   // 回滚：DOM 保持原样（已剪的 LNT 行面无损）
        return RwResult::EraseFail;
    }
    return RwResult::Ok;
}

/// 外部内建站点（parseUL）的实参收集：(String, int) 键形定死，压栈序
/// str·radix。String 实参 CP 直读——decodePush 栈值面对引用形保守拒（通用
/// 折叠的产物只有数值），内建站点把字符串当纯函数入参、产物仍是数值，
/// 不在该保守面内。producers 按压栈序（str 深、radix 浅），改写面直接用。
bool collectBuiltinArgsParseUl(ir::ClassSession& s, const ir::CodeDom& dom,
                               const std::vector<ir::InsnId>& order, ca::usize callPos,
                               std::vector<ir::InsnId>& producers,
                               std::vector<jsim::SimValue>& args)
{
    // 非NOP前驱定位（squeeze 遗留落点不驻栈，跳过）。
    const auto prevReal = [&](ca::usize from, ca::usize& out) {
        ca::usize k = from;
        while (k > 0) {
            --k;
            const ir::Insn* p = dom.peek(order[k]);
            if (p != nullptr && p->op != Op::NOP) {
                out = k;
                return p;
            }
        }
        return static_cast<const ir::Insn*>(nullptr);
    };

    // radix：紧邻 call 的 int 常量装载。
    ca::usize radixPos = 0;
    const ir::Insn* radix = prevReal(callPos, radixPos);
    if (radix == nullptr) return false;
    ca::i32 radixV = 0;
    if (!const_args::decodeConstInt(s, *radix, radixV)) return false;

    // str：再往前一条 LDC String。
    ca::usize strPos = 0;
    const ir::Insn* str = prevReal(radixPos, strPos);
    if (str == nullptr) return false;
    if (str->op != Op::LDC && str->op != Op::LDC_W) return false;
    ir::ClassSession::ConstSem sem;
    if (!s.constOf(*str, sem) || sem.kind != ir::ClassSession::ConstSem::Kind::String) {
        return false;
    }

    args.push_back(jsim::SimValue::knownStr(std::string(sem.str)));
    args.push_back(jsim::SimValue::knownI32(radixV));
    producers.push_back(order[strPos]);    // str：栈最深 → 折后原地锚
    producers.push_back(order[radixPos]);  // radix：受害面
    return true;
}

/// 在已 promote 的调用者方法里改写一个站点；失败不改动 DOM。
RwResult rewriteSite(ir::ClassSession& session, ir::CodeUnit& cu, const Site& site)
{
    if (site.isDecryptor) return rewriteDecryptorSite(session, cu, site);

    ir::CodeDom&                   dom   = cu.dom;
    const std::vector<ir::InsnId>& order = dom.order();

    ca::usize callPos = kNoIndex;
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = dom.peek(order[i]);
        if (in != nullptr && in->op == Op::INVOKESTATIC && in->srcPc == site.callSrcPc) {
            callPos = i;
            break;
        }
    }
    if (callPos == kNoIndex) return RwResult::CallNotFound;

    const ir::Insn* call = dom.peek(order[callPos]);
    ir::ClassSession::MemberSem enc;
    if (!session.memberOf(*call, enc)) return RwResult::MemberMismatch;
    if (enc.owner != site.targetOwner || enc.name != site.targetName ||
        enc.desc != site.targetDesc) {
        return RwResult::MemberMismatch;
    }

    std::vector<ir::InsnId>     producers;
    std::vector<jsim::SimValue> args;
    if (site.isExternalBuiltin) {
        // 内建站点：键形 (String, int)，String 实参 CP 直读（通用栈值面
        // 保守拒引用形，这里不走）。
        if (!collectBuiltinArgsParseUl(session, dom, order, callPos, producers, args)) {
            return RwResult::ArgsMismatch;
        }
    } else if (!const_args::collectArgs(session, dom, order, callPos,
                                        const_args::argSlots(enc.desc), producers, args)) {
        return RwResult::ArgsMismatch;
    }
    if (args.size() != site.args.size()) return RwResult::ArgsMismatch;
    for (ca::usize i = 0; i < args.size(); ++i) {
        if (!const_args::sameValue(args[i], site.args[i])) return RwResult::ArgsMismatch;
    }

    ir::Insn load;
    if (!rewrite_tools::makeConstLoad(session, site.value, load)) return RwResult::LoadFail;

    // order 是 DOM 的活视图：任何结构变更之后下标都会位移，先把句柄取出来。
    const ir::InsnId callId  = order[callPos];
    const ir::InsnId firstId = producers.front();

    // 区间里除首条以外的指令上不许有标签：有标签说明分支/switch/异常范围从
    // 区间中段进入，擦掉会让目标悬垂。首条不受此限，它原地改写，身份不变。
    for (ca::usize i = 1; i < producers.size(); ++i) {
        if (!dom.labelsAt(producers[i]).empty()) return RwResult::LabelBlocked;
    }
    if (!dom.labelsAt(callId).empty()) return RwResult::LabelBlocked;

    // 首条**原地改写**成常量装载，而不是前面插一条 + 删掉首条：
    // 插入路径把 srcPc 强制清成 kNoPc（合成指令没有旧位置，CodeDom 的硬
    // 不变量），而 LNT/LVT/StackMapTable 的重定位全靠旧 pc 命中 pcmap，
    // 删掉首条会让锚在区间起点的行全部失去落点，lower 只能整体失败。
    // 原地改写保住它的 srcPc，锚在它上面的标签也因此不必迁移。
    auto slotR = dom.get(firstId);
    if (slotR.is_err()) return RwResult::InsertFail;
    ir::Insn*      first   = slotR.unwrap();
    const ir::Insn original = *first;
    const ca::u32  keepPc   = first->srcPc;
    *first                  = load;
    first->srcPc            = keepPc;

    std::vector<ir::InsnId> victims(producers.begin() + 1, producers.end());
    victims.push_back(callId);

    if (std::getenv("GRUNT_DEBUG_PCREFS") != nullptr) {
        std::vector<ca::u32> dead;
        for (const ir::InsnId id : victims) {
            const ir::Insn* v = dom.peek(id);
            if (v != nullptr && v->srcPc != jbc::ir::kNoPc) dead.push_back(v->srcPc);
        }
        dumpPcRefs(session, site.callerMethodIndex, dead);
    }

    if (dom.eraseBatch(victims).is_err()) {
        *first = original;   // 回滚：DOM 保持原样
        return RwResult::EraseFail;
    }
    return RwResult::Ok;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

}   // namespace

// ============================================================================
// ==== 对外三段 ==============================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    scan_tools::MethodIndex index;
    buildIndex(container, index);

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("scan", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            ir::ClassSession&     s    = *session;
            const ir::ClassUnit&  unit = s.unit();
            const std::string_view self = s.backing().thisName();

            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                if (!ir::hasCode(s, mi)) continue;
                auto promoted = ir::promoteMethod(s, mi);
                if (promoted.is_err()) continue;
                ir::CodeUnit* cu = promoted.unwrap().get();
                if (cu == nullptr) continue;

                const std::string_view         name  = unit.symbols().str(unit.methods[mi].name);
                const std::string_view         desc  = unit.symbols().str(unit.methods[mi].desc);
                const std::vector<ir::InsnId>& order = cu->dom.order();

                for (ca::usize pos = 0; pos < order.size(); ++pos) {
                    const ir::Insn* in = cu->dom.peek(order[pos]);
                    if (in == nullptr || in->op != Op::INVOKESTATIC) continue;

                    ir::ClassSession::MemberSem enc;
                    if (!s.memberOf(*in, enc)) continue;
                    if (enc.kind != ir::ClassSession::MemberSem::Kind::Method) continue;

                    const auto reject = [&](RejectReason r) {
                        result.rejects.push_back(RejectedSite{
                            std::string(self), std::string(name), in->srcPc, r});
                    };

                    const auto it = index.find(scan_tools::keyOf(enc.owner, enc.name, enc.desc));
                    if (it == index.end()) {
                        // 容器外目标：唯一入表面 = JDK 全序纯内建白名单
                        // （kExternalPureBuiltins，登记规则见表注）。求值在
                        // evaluateSites 头部按成员直折。
                        if (!isExternalPureBuiltin(enc.owner, enc.name, enc.desc)) {
                            continue;
                        }
                        std::vector<ir::InsnId>     producers;
                        std::vector<jsim::SimValue> args;
                        if (!collectBuiltinArgsParseUl(s, cu->dom, order, pos,
                                                       producers, args)) {
                            reject(RejectReason::ArgsNotConstant);
                            continue;
                        }
                        Site site;
                        site.callerUri         = std::string(entry.uri);
                        site.callerOwner       = std::string(self);
                        site.callerMethod      = std::string(name);
                        site.callerDesc        = std::string(desc);
                        site.callerMethodIndex = mi;
                        site.callSrcPc         = in->srcPc;
                        site.targetOwner       = std::string(enc.owner);
                        site.targetName        = std::string(enc.name);
                        site.targetDesc        = std::string(enc.desc);
                        site.args              = std::move(args);
                        site.isExternalBuiltin = true;
                        result.sites.push_back(std::move(site));
                        continue;
                    }
                    const scan_tools::MethodLoc& loc = it->second;

                    if ((loc.access & kAccStatic) == 0) {
                        reject(RejectReason::TargetNotStatic);
                        continue;
                    }
                    if ((loc.access & kAccSynchronized) != 0) {
                        reject(RejectReason::TargetSynchronized);
                        continue;
                    }
                    if (!enc.name.empty() && enc.name.front() == '<') {
                        reject(RejectReason::TargetInitializer);
                        continue;
                    }
                    if (returnChar(enc.desc) == 'V') {
                        reject(RejectReason::TargetVoid);
                        continue;
                    }

                    // 解密器族分派：形态 + 同类（JVMS 5.5）双门，命中即自成
                    // 站点（不走通用实参收集——toCharArray 前缀必然拒）。
                    if (enc.desc == "([CJI)Ljava/lang/String;" && enc.owner == self) {
                        DecryptorShape shape;
                        if (matchDecryptorShape(s, cu->dom, order, pos, shape)) {
                            Site site;
                            site.callerUri         = std::string(entry.uri);
                            site.callerOwner       = std::string(self);
                            site.callerMethod      = std::string(name);
                            site.callerDesc        = std::string(desc);
                            site.callerMethodIndex = mi;
                            site.callSrcPc         = in->srcPc;
                            site.targetOwner       = std::string(enc.owner);
                            site.targetName        = std::string(enc.name);
                            site.targetDesc        = std::string(enc.desc);
                            site.targetMethodIndex = loc.index;
                            site.isDecryptor       = true;
                            site.cipherUnits       = std::move(shape.cipherUnits);
                            site.key               = shape.key;
                            site.seed              = shape.seed;
                            result.sites.push_back(std::move(site));
                            continue;
                        }
                    }

                    std::vector<ir::InsnId>     producers;
                    std::vector<jsim::SimValue> args;
                    if (!const_args::collectArgs(s, cu->dom, order, pos,
                                                 const_args::argSlots(enc.desc),
                                                 producers, args)) {
                        reject(RejectReason::ArgsNotConstant);
                        continue;
                    }

                    Site site;
                    site.callerUri         = std::string(entry.uri);
                    site.callerOwner       = std::string(self);
                    site.callerMethod      = std::string(name);
                    site.callerDesc        = std::string(desc);
                    site.callerMethodIndex = mi;
                    site.callSrcPc         = in->srcPc;
                    site.targetOwner       = std::string(enc.owner);
                    site.targetName        = std::string(enc.name);
                    site.targetDesc        = std::string(enc.desc);
                    site.targetMethodIndex = loc.index;
                    site.args              = std::move(args);
                    result.sites.push_back(std::move(site));
                }
            }
        });
    });

    evaluateSites(container, result.sites, result.rejects);
    return result;
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    // 按调用类分组：一次会话内改完该类的全部站点，再 lower + 回写。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& s : result.sites) byUri[s.callerUri].push_back(&s);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("rewrite", entry.uri, [&] {
        const auto it = byUri.find(std::string(entry.uri));
        if (it == byUri.end()) return;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::unordered_map<ca::usize, std::vector<const Site*>> byMethod;
        for (const Site* site : it->second) byMethod[site->callerMethodIndex].push_back(site);

        // 探针必须看到未改动的字节，所以整类的自检先于任何 promote/改写。
        std::vector<ca::usize> candidates;
        candidates.reserve(byMethod.size());
        for (const auto& slot : byMethod) candidates.push_back(slot.first);
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(*entry.bytes, candidates);

        bool changed = false;
        for (auto& [methodIndex, sites] : byMethod) {
            if (safe.find(methodIndex) == safe.end()) {
                dirty.skippedByRoundTrip += sites.size();
                continue;
            }
            auto promoted = ir::promoteMethod(s, methodIndex);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            bool methodChanged = false;
            for (const Site* site : sites) {
                const RwResult r = rewriteSite(s, *cu, *site);
                ++dirty.attempted;
                if (r == RwResult::Ok) {
                    ++dirty.applied;
                    methodChanged = true;
                    if (site->isDecryptor && std::getenv("GRUNT_DEBUG_FOLD") != nullptr) {
                        // 解密器逐条落折：cipher/key/seed → 明文，与 JVM 反射
                        // 求值对照。
                        std::fprintf(stderr, "[fold] %s.%s pc=%u -> %s cipher=%zu key=%lld seed=%d => \"%s\"\n",
                                     site->callerOwner.c_str(), site->callerMethod.c_str(),
                                     site->callSrcPc, site->targetName.c_str(),
                                     site->cipherUnits.size(),
                                     static_cast<long long>(site->key), site->seed,
                                     site->plaintext.c_str());
                    }
                    if (std::getenv("GRUNT_DEBUG_FOLD") != nullptr && !site->isDecryptor) {
                        // 逐条落折成了什么：用来和 JVM 反射求值对照。
                        std::fprintf(stderr, "[fold] %s.%s pc=%u -> %s  = ", site->callerOwner.c_str(),
                                     site->callerMethod.c_str(), site->callSrcPc,
                                     site->targetName.c_str());
                        for (const jsim::SimValue& arg : site->args) {
                            switch (arg.type) {
                            case jsim::ValType::I32:
                                std::fprintf(stderr, "%d ", static_cast<ca::i32>(arg.intBits));
                                break;
                            case jsim::ValType::I64:
                                std::fprintf(stderr, "%lld ", static_cast<long long>(arg.intBits));
                                break;
                            case jsim::ValType::Ref:
                                if (arg.ref.kind == jsim::RefPayload::Kind::Null) {
                                    std::fprintf(stderr, "null ");
                                } else {
                                    std::fprintf(stderr, "? ");
                                }
                                break;
                            default: std::fprintf(stderr, "? "); break;
                            }
                        }
                        switch (site->value.type) {
                        case jsim::ValType::I32:
                            std::fprintf(stderr, "=> %d\n",
                                         static_cast<ca::i32>(site->value.intBits));
                            break;
                        case jsim::ValType::I64:
                            std::fprintf(stderr, "=> %lld\n",
                                         static_cast<long long>(site->value.intBits));
                            break;
                        default: std::fprintf(stderr, "=> ?\n"); break;
                        }
                    }
                } else {
                    const ca::usize idx = static_cast<ca::usize>(r);
                    if (dirty.failedByReason.size() <= idx) dirty.failedByReason.resize(idx + 1, 0);
                    ++dirty.failedByReason[idx];
                }
            }
            if (!methodChanged) continue;
            auto lowered = ir::lowerMethod(s, methodIndex);
            if (lowered.is_err()) {
                if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                    std::fprintf(stderr, "[lower-fail] %s method#%zu err=%s\n",
                                 std::string(s.backing().thisName()).c_str(), methodIndex,
                                 ir::to_string(lowered.unwrap_err()));
                }
                return;   // 整类放弃
            }
            changed = true;
        }
        if (!changed) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                std::fprintf(stderr, "[write-fail] %s err=%s\n",
                             std::string(s.backing().thisName()).c_str(),
                             jbc::core::to_string(encoded.unwrap_err()));
                // 池面诊断：区分 CP 引用失效 vs 属性层编码失败。
                jbc::core::Diagnostics diag;
                if (s.backing().cp.validateReferences(diag).is_err()) {
                    for (const jbc::core::Diagnostic& d : diag.items()) {
                        std::fprintf(stderr, "[write-diag] cp-ref %s off=%u\n",
                                     jbc::core::to_string(d.code), d.offset);
                    }
                } else {
                    std::fprintf(stderr, "[write-diag] cp refs OK — 失败在属性编码层\n");
                }
            }
            return;
        }
        pending.push_back(PendingWrite{std::string(entry.uri), std::move(encoded).unwrap()});
        });
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
    }
    return dirty;
}

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    ProofResult result;
    if (countClasses(container) != classesBefore) {
        result.ok = false;
        result.failed.emplace_back("class-count");
    }
    for (const std::string& uri : dirty.uris) {
        bool found = false;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (found || entry.uri != uri) return;
            found = openSession(*entry.bytes) != nullptr;
        });
        if (!found) {
            result.ok = false;
            result.failed.push_back(uri);
        }
    }
    return result;
}

}   // namespace grunt::constant_call
