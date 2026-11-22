/// @file        junk_member_strip.hpp
/// @brief       残留 `junk_member_strip`：junk 键初始化段 NOP 化 + junk 成员
///              摘除（用户 PR #14 收尾项）。
///
/// ## 形态（artifact 侧，dec_jdk.jar 655 类普查实证）
///
/// Grunteon 混淆的 jsoup 产物在解密链收尾后残留 junk 成员层：1040 个 `$g`
/// 形字段（`$g[a|b|k|p]<13位小写数字>`，private static，J/I）+ 41 个数字开头
/// 形字段（字符串池数组）+ 260 个 `$gg` 形方法（`$gg<13位>`，private static
/// `()I`——反调试环境探针：体内检查 jdwp/-noverify/JAVA_TOOL_OPTIONS 等）。
/// 这些成员只被 `<clinit>`/`<init>` 里的 junk 键初始化段引用：ldc2_w 常量 +
/// nop 对 + ladd/lsub/lxor/ior 混算 + invokestatic 调探针方法 + ifeq/goto
/// 条件 trampoline。探针返回值经 dup/ifeq 分发：一条路径落到真实初始化体
/// （通常经 goto 跳回），另一条是 junk 路径（诱饵 API 调用 + athrow 陷阱 +
/// 提前 return 的反调试 trap）。普查实测两 jar 均为 1081 字段 + 260 方法。
///
/// ## 判据（scan 的细找）
///
/// 逐方法（不限 clinit/init）三步：
///   1. 栈深模拟：从方法入口（深度 0）与全部异常 handler（深度 1）出发，
///      按操作码槽位效应表（字段/调用按描述符解析 J/D 双槽）走查。wide /
///      jsr/ret / 表外操作码 / 槽位下溢 / 合流不匹配 → 整方法放弃。
///   2. junk CFG 闭包：种子 = 引用 junk 形成员的指令（junk 字段
///      GETSTATIC/PUTSTATIC、junk 方法 INVOKESTATIC，owner 须为本类）。
///      从种子沿 CFG 边（直落 + 分支目标）只在**白名单指令**上扩张；再对
///      闭包成员做**直落前驱回拉**（putstatic 种子消费的 ldc2_w/nop 馈送
///      前缀在正向闭包上游），到不动点：
///      - nop/常量装载（ldc 仅数值载荷）/局部 load/store/栈操作/算术转换
///        比较/iinc；
///      - 区域内跳转（goto/if/switch——目标是否在闭包内由闭包本身判定）；
///      - `return`/`athrow`（athrow 是 junk 陷阱路径，NOP 化即拆陷阱）；
///      - invokestatic→本类 junk 方法；GETSTATIC/PUTSTATIC→本类 junk 字段
///        （GETSTATIC 是普查实证的必需项：1081 个 junk 字段全部有区域内的
///        读点喂给 ladd/lsub/lxor/ior）；
///      - 扩张（tier-2，普查背书）：GETSTATIC/INVOKESTATIC→真实成员且
///        **下一条指令恰为 pop/pop2**——trampoline 里撒的诱饵读/调
///        （Locale.ENGLISH、System.inheritedChannel 形），值被立即丢弃。
///   3. 改写可行性复核（全部在 scan 定死，rewrite 只按定死的段写 0x00）：
///      - 前沿收缩：改写后模拟（闭包→NOP）在非闭包指令上槽位下溢/合流
///        不匹配时，把喂值给失败点的闭包指令（非种子）退出闭包，重模拟
///        （≤64 轮；收敛不了整方法放弃）——aload 喂真实调用的缝隙面；
///      - 孤儿容许：改写后不可达的指令（原可达→改写后不可达）若整集都是
///        诱饵形（无局部读、无 new/invokespecial/putfield/数组写/monitor/
///        wide/indy、putstatic 仅 junk 字段、ldc 仅数值），容许其成为死
///        字节（反调试 trap 路径的归宿）；含真实初始化签名指令 → 整方法
///        放弃（DocumentType 构造体挂 junk 门后的形态，实证）；
///      - 夹层真实块 ≤ 1（多块时改写后执行序由 pc 序决定，原序不可证 →
///        宁放弃）；异常 handler 落在闭包内 → 整方法放弃（handler 入口的
///        帧锚不可丢）；SMT 解不开 → 整方法放弃。
///      - 四道结构门（真实调用者/读者保护面，夹具拒证）：G1 junk 命名方法
///        只作删除候选不做区域宿主；G2 区域不得从方法入口起（须有真实前
///        缀）；G3 区域须有 ≥1 junk 字段种子（键状态机证据）；G4 值馈送
///        回拉只收净槽位效应 ≥ 0 的整链（链不许消费闭包外的栈值）。
///
/// ## 改写（rewrite）
///
/// 四步（对齐 dead_member 的压缩-普查-删除-压缩骨架）：
///   1. 逐类按 scan 定死的段把闭包指令原位写 0x00（**长度保持**——code
///      长度、全部指令偏移、LVT/LNT 不动），并按 (F) 面核对 SMT 帧：锚在
///      段内的帧摘除（栈深/locals 内容与改写后计算态一致的 full 帧保留，
///      它承载 locals 类型态基线），跨被摘帧重排后续帧的 offset_delta，
///      same/same1 帧超界升格 same_frame_extended /
///      same_locals_1_stack_item_extended；保留尾 return 及其后的陷阱尾
///      （pop/goto 回跳保留尾的蹦床）不 NOP——其帧声明状态对未改动指令流
///      成立，摘掉会让 locals 基线塌回 uninitializedThis（JVM 实证）；
///   2. 候选类 CP 压缩（注解深度预检，失败类原样保真入失败集）——NOP 后
///      死 ref 从池里带走，普查前提的池闭包成立；非候选类不搅动（干净类
///      字节零变化是验收面）；
///   3. 活引用普查（分层兜底：会话 → 原始字节骨架 → census-blind 整轮
///      放弃）+ 池明文名字守卫：junk 成员名字出现在任何字符串常量里不删；
///   4. 逐类倒序 erase 删净 junk 方法/字段（引用还活着的成员宁留，记
///      rejectedByRef），再压缩一次带走描述符。residual = 容器里剩余
///      junk 形成员数（被保守面挡下的部分）。
///
/// ## 帧面（JVM 验证实证，HotSpot 逐错钉出的五道核对）
///
/// NOP 化改动控制流后，校验器对 SMT 的要求比直觉严：
///   - F1 摘除集按 **段** 计（非闭包）——陷阱尾/保留尾的帧保留；段内 full
///     帧在「栈深一致 + locals 内容 == 初始内容 + 其前无真实 locals 写」
///     三条全满足时保留（locals 类型态基线由它承载）；
///   - F2 保留帧若改写后可达，声明栈深必须等于改写后槽深（真实块入口帧
///     声明探针旧栈形 → Inconsistent stackmap）；
///   - F3 被摘帧若携带 locals 态变化，其后可达保留帧差分基线偏离真态 →
///     整方法放弃（locals 内容沿原链与保留链双轨对照）；
///   - F4 掩蔽段外的真实分支不得指向帧被摘的 pc（Expecting a stackmap
///     frame at branch target）；
///   - 保留尾 return 是无条件转移，其后的陷阱尾首指令是语法块头，帧必须
///     在位（Expecting a stack map frame）——保留陷阱尾即满足。
///
/// ## 联动
///
/// 删净成员后伴生类（`$PredicateProcessor$` 形）失去引用，由既有
/// `grunt.DeadMember` 空壳回收接管；本 pass 不摘类，只负责把「删净成员」
/// 的状态交给它。
///
/// ## 统计与 residual（dec_jdk.jar 全量 655 类实测，2026-11-22）
///
/// 单跑 dec_jdk.jar：129 个 NOP 区域 / 8/260 junk 方法与 32/1081 junk 字段
/// 进删除面（residual 252 方法 + 1049 字段）。验收面：全量类 JVM 链接 +
/// clinit 初始化 ok=655 fail=0；jsoup 对拍 14 语料逐字节一致。残差主因：
/// `<init>`/`<clinit>` 里探针返回值与真实初始化体的**栈耦合**（改写后模拟
/// underflow，211 方法）+ indy 体效应未知（65）+ G2 入口起区域（47）+
/// 夹层多真实块（33）+ 帧面核对（F2/F3/F4 合计 ~120）+ kept-return 槽深
/// （15）。与四步原型（278 区域/53 探针/212 字段）的差额全部在上述保守面
/// ——删除集是原型的子集，宁留方向。v3_full_obf（混淆原始输入）字段全部
/// 有真实读者、整层不删，260 个探针方法零调用点全删（纯死码）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::junk_member_strip {

/// @brief 一个方法的 NOP 化区域（scan 定死，rewrite 只写 0x00 不回看判据）。
struct JunkRegion
{
    ca::usize methodIndex = 0;   ///< backing 方法序号
    /// @brief NOP 化字节段（闭包指令的 [pc, pc+len) 造段，已并相邻段）。
    ///        保留尾 return 及其后的陷阱尾（pop/goto 回跳保留尾的蹦床）不在
    ///        段内——陷阱尾保持原指令原帧原状态，是合法的迂回出口。
    std::vector<std::pair<ca::u32, ca::u32>> spans;
    /// @brief 锚在 NOP 化段内 pc 上的 SMT 帧（rewrite 摘除并重排 delta）。
    ///        陷阱尾与保留尾的帧原样保留（其声明状态对原指令流成立）。
    std::vector<ca::u32> droppedFramePcs;
    /// @brief NOP 化段内的 athrow 条数（陷阱拆除计数，proof 侧仅统计）。
    ca::usize athrowCount = 0;
};

/// @brief 一类候选：NOP 区域 + junk 形成员（成员删除面，引用检查在 rewrite）。
struct ClassCand
{
    std::string             uri;
    std::string             owner;
    std::vector<JunkRegion> regions;
    std::vector<ca::usize>  methodIdxs;   ///< junk 形方法 backing 序号
    std::vector<ca::usize>  fieldIdxs;    ///< junk 形字段 backing 序号
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<ClassCand> classes;
    ca::usize              classesSkipped   = 0;   ///< 会话打不开的类
    ca::usize              methodsSimFail   = 0;   ///< 栈模拟放弃的方法
    ca::usize              methodsFailClosed = 0;  ///< 其余保守面放弃的方法
    ca::usize              regionsTotal     = 0;   ///< 收进的 NOP 区域数
    ca::usize              seedsTotal       = 0;   ///< junk 引用种子指令数
    ca::usize              orphanInsns      = 0;   ///< 容许孤儿的诱饵指令数
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;           ///< 落过盘的类（NOP/压缩/删除）
    ca::usize                regions  = 0;   ///< NOP 化区域数
    ca::usize                nopBytes = 0;   ///< NOP 化字节数
    ca::usize                athrows  = 0;   ///< 拆除的 athrow 陷阱数
    ca::usize                methods  = 0;   ///< 删除的 junk 方法数
    ca::usize                fields   = 0;   ///< 删除的 junk 字段数
    ca::usize                compacted        = 0;   ///< 仅压缩变小的类数
    ca::usize                attempted        = 0;   ///< 进引用检查的成员数
    ca::usize                rejectedByRef    = 0;   ///< 引用还活着的成员数
    ca::usize                censusRawSources = 0;   ///< 普查走原始字节路径的类数
    ca::usize                censusBlind      = 0;   ///< 骨架都解不开的类数（>0 整轮放弃）
    ca::usize                residualMethods  = 0;   ///< 容器剩余 junk 形方法数
    ca::usize                residualFields   = 0;   ///< 容器剩余 junk 形字段数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residualMethods = 0;
    ca::usize                residualFields  = 0;
};

/// @brief SCAN：纯读。junk 形成员普查 + 逐方法区域判定。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：NOP 化 → 压缩 → 普查 → 删成员 → 再压缩 → residual 记账。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 落盘类可重解析 + residual 计数核对。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::junk_member_strip
