/// @file        number_fold.cpp
/// @brief       残留 `number_fold` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/number_fold.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/iter_budget.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

#include "jsim/value.hpp"

namespace grunt::number_fold {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using const_args::ValueLoad;
using proof_tools::roundTripSafe;

/// 基 32 数字：0-9 = 0..9，a-v / A-V = 10..31（java.lang.Long.toString(v, 32)
/// 的生成口径）。
int base32Digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'v') return c - 'a' + 10;
    if (c >= 'A' && c <= 'V') return c - 'A' + 10;
    return -1;
}

/// @brief 严格基 32 解析：java.lang.Long.parseUnsignedLong 的生成侧逆。
///
/// 只认无符号数字前缀串（生成侧 Long.toString(v, 32) 不产出符号/空白），
/// 任一字符越出 radix 或总和超 64 位即拒绝（fail-closed）。
bool parseBase32(std::string_view text, ca::u64& out)
{
    if (text.empty()) return false;
    ca::u64 acc = 0;
    for (const char c : text) {
        const int d = base32Digit(c);
        if (d < 0) return false;
        const ca::u64 limit = (static_cast<ca::u64>(-1) - static_cast<ca::u64>(d)) / 32;
        if (acc > limit) return false;
        acc = acc * 32 + static_cast<ca::u64>(d);
    }
    out = acc;
    return true;
}

/// @brief 包装调用的引用判据：INVOKESTATIC 且 owner/name/desc 精确吻合。
bool isWrapperInvoke(ir::ClassSession& s, const ir::Insn& in, std::string_view owner,
                     std::string_view name, std::string_view desc)
{
    if (in.op != Op::INVOKESTATIC) return false;
    ir::ClassSession::MemberSem sem;
    if (!s.memberOf(in, sem)) return false;
    return sem.kind == ir::ClassSession::MemberSem::Kind::Method
           && sem.owner == owner && sem.name == name && sem.desc == desc;
}

/// @brief 跨度：order 区间 [firstPos..lastPos] + 折叠值 + 尾序号。
struct Span
{
    ca::usize firstPos = 0;   ///< 锚点（跨度首条装载）在 order 里的位置
    ca::usize lastPos  = 0;   ///< 跨度尾条在 order 里的位置
    ca::usize tailIdx  = 0;   ///< 尾条在实参序号（收集序）里的下标
    ValueLoad value;          ///< 折叠结果
    /// 通用跨度改写口径：受害者 NOP 化而非擦除。透明 goto 的目标 ControlFlow
    /// 标签锚在跨度内部成员上，eraseBatch 连标签一起清场后标签悬垂，lower 的
    /// pcmap 重定位对悬垂锚报 relocation bad target（拼装走廊实证）；NOP 化
    /// 保住全部旧 pc，SMT/LNT/异常表原地落点不变，遗留 NOP 由 DeadCode 收尾。
    bool nopVictims = false;
};

constexpr ca::usize kMaxSpan = 7;   ///< 最长跨度（int 跨度 6 条 + float 包装 1 条）

/// @brief 从 from 起收集最多 kMaxSpan 条非 NOP 指令（NOP 是 squeeze 遗留落点，
///        不驻栈，跳过，与 const_args::collectArgs 同一口径）。
void collectReal(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                 ca::usize from, std::vector<ca::usize>& pos,
                 std::vector<const ir::Insn*>& in)
{
    pos.clear();
    in.clear();
    for (ca::usize p = from; p < order.size() && in.size() < kMaxSpan; ++p) {
        const ir::Insn* cur = dom.peek(order[p]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        pos.push_back(p);
        in.push_back(cur);
    }
}

// ---- 跨度形状（逐条钉死；同锚点各形状第二指令 op 分叉，站点跨度天然不重叠）--

/// int 跨度 1：<i32 a> I2L <i32 b> I2L LXOR L2I → a ^ b。
///
/// I2L/L2I 走廊是纯噪声：符号扩展保低 32 位，XOR 按位运算，截断回 i32 与
/// plain 异或等价。判据仍按跨度语义取值（先符号扩展再异或再截断）。
bool matchIntViaLong(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                     const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 6) return false;
    ValueLoad b;
    if (in[1]->op != Op::I2L) return false;
    if (!const_args::decodeValueLoad(s, *in[2], b) || b.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (in[3]->op != Op::I2L || in[4]->op != Op::LXOR || in[5]->op != Op::L2I) return false;
    const ca::i64 x = static_cast<ca::i64>(out.value.i32());
    const ca::i64 y = static_cast<ca::i64>(b.i32());
    out.firstPos = pos[0];
    out.lastPos  = pos[5];
    out.tailIdx  = 5;
    out.value.kind  = ValueLoad::Kind::I32;
    out.value.bits  = static_cast<ca::u64>(static_cast<ca::i64>(x ^ y));
    out.value.slots = 1;
    return true;
}

/// int 跨度 2：<i64 a> L2I <i32 b> IXOR → (int)a ^ b。
bool matchLongToInt(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                    const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 4) return false;
    ValueLoad b;
    if (in[1]->op != Op::L2I) return false;
    if (!const_args::decodeValueLoad(s, *in[2], b) || b.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (in[3]->op != Op::IXOR) return false;
    const ca::i32 x = static_cast<ca::i32>(out.value.i64());
    const ca::i32 y = b.i32();
    out.firstPos = pos[0];
    out.lastPos  = pos[3];
    out.tailIdx  = 3;
    out.value.kind  = ValueLoad::Kind::I32;
    out.value.bits  = static_cast<ca::u64>(static_cast<ca::i64>(x ^ y));
    out.value.slots = 1;
    return true;
}

/// int 位拼（v2 NumberEncrypt Int 第二式 + junk 同形）：
/// <i32 a> <i32 b> IAND → a & b。
///
/// 位与是纯位运算：i32 逐位与与 JVM IAND 逐位等价，无符号性/溢出无涉；
/// 折叠值直接取 a & b 的位形，不依赖生成侧first & second = 原值的构造
/// 意图。第二操作数类型钉 I32（I64/LDC2_W 喂给 = 栈形破裂，拒绝）。
bool matchIntSplice(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                    const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 3) return false;
    ValueLoad b;
    if (!const_args::decodeValueLoad(s, *in[1], b) || b.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (in[2]->op != Op::IAND) return false;
    const ca::i32 x = out.value.i32();
    const ca::i32 y = b.i32();
    out.firstPos = pos[0];
    out.lastPos  = pos[2];
    out.tailIdx  = 2;
    out.value.kind  = ValueLoad::Kind::I32;
    out.value.bits  = static_cast<ca::u64>(static_cast<ca::i64>(x & y));
    out.value.slots = 1;
    return true;
}

/// long 位拼（v2 NumberEncrypt Long 全式 + junk 同形）：
/// <i64 a> <i64 b> LAND → a & b。64 位逐位与，与 JVM LAND 逐位等价；
/// 第二操作数类型钉 I64。
bool matchLongSplice(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                     const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 3) return false;
    ValueLoad b;
    if (!const_args::decodeValueLoad(s, *in[1], b) || b.kind != ValueLoad::Kind::I64) {
        return false;
    }
    if (in[2]->op != Op::LAND) return false;
    const ca::i64 x = out.value.i64();
    const ca::i64 y = b.i64();
    out.firstPos = pos[0];
    out.lastPos  = pos[2];
    out.tailIdx  = 2;
    out.value.kind  = ValueLoad::Kind::I64;
    out.value.bits  = static_cast<ca::u64>(x & y);
    out.value.slots = 2;
    return true;
}

/// long 跨度：LDC "key" <i32 32> Long.parseUnsignedLong(String,I)J <i64 obf>
/// LXOR → key ^ obf（无符号位形异或）。
bool matchBase32Long(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                     const std::vector<const ir::Insn*>& in, Span& out)
{
    if (in.size() < 5) return false;
    ValueLoad radix;
    ValueLoad obf;
    if (!const_args::decodeValueLoad(s, *in[1], radix) || radix.kind != ValueLoad::Kind::I32) {
        return false;
    }
    if (radix.i32() != 32) return false;   ///< 家族生成面恒 Long.toString(v, 32)
    if (!isWrapperInvoke(s, *in[2], "java/lang/Long", "parseUnsignedLong",
                         "(Ljava/lang/String;I)J")) {
        return false;
    }
    if (!const_args::decodeValueLoad(s, *in[3], obf) || obf.kind != ValueLoad::Kind::I64) {
        return false;
    }
    if (in[4]->op != Op::LXOR) return false;

    ca::u64 key = 0;
    if (!parseBase32(out.value.str, key)) return false;
    out.firstPos = pos[0];
    out.lastPos  = pos[4];
    out.tailIdx  = 4;
    out.value.kind  = ValueLoad::Kind::I64;
    out.value.bits  = key ^ static_cast<ca::u64>(obf.i64());
    out.value.str.clear();
    out.value.slots = 2;
    return true;
}

constexpr ca::usize kMaxGenericSpan = 64;   ///< 通用封闭跨度收集窗（拼装走廊实测 ≤ 25 条实体指令）

/// 通用跨度的标签普查：跨度内（锚点除外）锚定的标签，引用者全部落在跨度
/// 自身（空转 goto，随跨度一并擦除）时不是合并点；任何外部引用（别的分支
/// /switch 命中）= 合并点，整站放弃。junk 直落跳（goto 紧随指令）的目标
/// 标签就绑在跨度内下一条指令上，严格合并点口径会把整条走廊误杀。
bool genericLabelsOk(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                     const std::vector<ca::usize>& pos, const std::vector<ir::InsnId>& inIds,
                     ca::usize firstPos, ca::usize lastPos)
{
    using jbc::ir::LabelId;
    // 内部标签普查只看跨度区间 (firstPos..lastPos]：收集窗（≤64 条实体指令）
    // 常大于跨度（最长封闭前缀停在首个外部边界），窗外指令的标签——比如紧邻
    // 下一条走廊的 goto 目标——不是本跨度的合并点，把它们算进来会让合法的
    // 窗外引用者（下一走廊自己的空转 goto）误杀本跨度（b.main 实证）。
    std::vector<LabelId> interior;
    for (ca::usize k = 1; k < inIds.size(); ++k) {
        if (pos[k] <= firstPos || pos[k] > lastPos) continue;
        for (const LabelId l : dom.labelsAt(inIds[k])) interior.push_back(l);
    }
    if (interior.empty()) return true;
    for (ca::usize p = 0; p < order.size(); ++p) {
        const ir::Insn* b = dom.peek(order[p]);
        if (b == nullptr) continue;
        const bool refsTarget =
            std::find(interior.begin(), interior.end(), b->target) != interior.end();
        const bool refsSwitch = std::any_of(
            b->targets.begin(), b->targets.end(), [&](LabelId l) {
                return std::find(interior.begin(), interior.end(), l) != interior.end();
            });
        if (!refsTarget && !refsSwitch) continue;
        // 引用者落在跨度区间内 = 自引用（空转 goto），随跨度消亡；区间外 =
        // 外部到达路径，合并点。
        if (p <= firstPos || p > lastPos) {
            // 诊断面（GRUNT_NF_TRACE）：本 pass 的 trace 走 stderr，实证会扰
            // 动分配次序，个别方法在 trace 口径下 lower 出现生产口径没有的
            // RelocationBadTarget（生产口径零 fail）——trace 行只作定位参考，
            // 下结论以无 env 的干净跑为准。
            if (std::getenv("GRUNT_NF_TRACE") != nullptr) {
                const ir::Insn* an = dom.peek(order[firstPos]);
                std::fprintf(stderr,
                             "[nf-labels] extref refPos=%zu refPc=%u refOp=%d span=[%zu,%zu] anchorPc=%u\n",
                             p, b->srcPc, static_cast<int>(b->op), firstPos, lastPos,
                             an != nullptr ? an->srcPc : 0u);
            }
            return false;
        }
    }
    return true;
}

/// 通用封闭跨度（命名形状全拒后的兜底）：纯常量封闭计算的**最长封闭前缀**。
///
/// 拼装走廊族（Grunt v3 池填充）：`parseUnsignedLong(key) ^ ((int跨度 i2l
/// mask land 32 lshl) | (int跨度 i2l mask land))`，高/低位两半生成面顺序可
/// 交换，goto/NOP 夹层任意——形状表组合爆炸，改钉**结构事实**：跨度由纯常
/// 量装载与封闭运算构成，栈机求值即静态事实。
///
/// 词汇表 = const_args::decodeValueLoad 的常量装载（含字符串键）、数值转换
/// 面选取（精确/最近舍入族；F2I/F2L/D2I/D2L 的截断-NaN-饱和边不收——家
/// 族不产）、整型/长整型算术位运算与移位（不含除/余——可抛
/// ArithmeticException，超出封闭面）与三个精确吻合的包装调用
/// （parseUnsignedLong 的 radix 恒 32，家族生成面事实）。栈机从空栈模型
/// 走，两类走停口径不同：
/// - **词汇内失败（欠栈/类别不符）= 硬拒**：跨度值正被词汇族运算喂给跨度
///   之外的操作数——封闭面破缺（非常量操作数走廊），整锚拒绝，不落更短
///   归点（更短前缀的"折叠"会把外值耦合进产出语义，宁不折）；
/// - **词汇外边界（家族外指令/窗尾）= 落最后一个实体指令 ≥ 2 的深度归点**
///   （最长封闭前缀；更短的前缀同样封闭，但整链重复跑逐段收敛不划算，一
///   次收整条走廊；裸锚点不算归点——单条常量的"折叠"是自等恒等，还淹站
///   点面）。
/// 产出必须落在数值四形（Str 归点 = 字符串走廊不折，引用身份不可静态构
/// 造）。
bool matchGenericSpan(ir::ClassSession& s, const ir::CodeDom& dom,
                      const std::vector<ir::InsnId>& order, ca::usize anchorPos,
                      Span& out)
{
    std::vector<ca::usize>       pos;
    std::vector<const ir::Insn*> in;
    std::vector<ir::InsnId>      inIds;
    for (ca::usize p = anchorPos; p < order.size() && in.size() < kMaxGenericSpan; ++p) {
        const ir::Insn* cur = dom.peek(order[p]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        pos.push_back(p);
        in.push_back(cur);
        inIds.push_back(order[p]);
    }
    if (in.size() < 2) return false;

    std::vector<ValueLoad> stack;
    ca::usize              closedTail  = static_cast<ca::usize>(-1);   ///< 最后一个可用深度归一点（in 下标 ≥ 1），npos = 无
    ValueLoad              closedValue{};
    bool                   broken      = false;   ///< 词汇内失败（欠栈/类别不符）：硬拒口径

    const auto popIf = [&](ValueLoad::Kind want, ValueLoad& v) -> bool {
        if (stack.empty() || stack.back().kind != want) {
            broken = true;   ///< 词汇内失败：跨度值被喂给跨度之外的操作数
            return false;
        }
        v = stack.back();
        stack.pop_back();
        return true;
    };
    const auto stopHere = [&]() -> bool {
        const bool nfTrace = std::getenv("GRUNT_NF_TRACE") != nullptr;
        const auto anchorPc = [&]() -> ca::u32 {
            const ir::Insn* an = dom.peek(order[anchorPos]);
            return an != nullptr ? an->srcPc : 0u;
        };
        if (closedTail == static_cast<ca::usize>(-1)) {
            if (nfTrace) {
                // broken（词汇内破缺）与 noclosed（纯开放）都收在无归点一条里：
                // 没有归点就没有可折的纯前缀。
                std::fprintf(stderr, "[nf-gen] %s %s pc=%u span=%zu\n",
                             broken ? "broken" : "noclosed",
                             std::string(s.backing().thisName()).c_str(), anchorPc(),
                             in.size());
            }
            return false;
        }
        // 词汇内破缺（欠栈/类别不符）不再整站硬拒：已落归点的前缀是纯走廊
        // 局部计算（空栈模型起走，每步都在走廊局部栈上成功），失败只说明
        // 后续词汇运算在消费外部操作数（dadd 配局部变量、ixor 配 iload）——
        // 封闭前缀照折，消费侧留在原地，栈形等价。无归点才是硬拒口径。
        if (closedValue.kind == ValueLoad::Kind::None
            || closedValue.kind == ValueLoad::Kind::Str) {
            if (nfTrace) {
                std::fprintf(stderr, "[nf-gen] badkind %s pc=%u kind=%d\n",
                             std::string(s.backing().thisName()).c_str(), anchorPc(),
                             static_cast<int>(closedValue.kind));
            }
            return false;
        }
        out.firstPos = pos[0];
        out.lastPos  = pos[closedTail];
        out.tailIdx  = closedTail;
        out.value    = closedValue;
        out.value.str.clear();
        // 标签普查（见 genericLabelsOk）：严格合并点口径会误杀空转 goto 走廊。
        if (!genericLabelsOk(dom, order, pos, inIds, out.firstPos, out.lastPos)) {
            if (nfTrace) {
                std::fprintf(stderr, "[nf-gen] labels %s pc=%u lastPos=%zu\n",
                             std::string(s.backing().thisName()).c_str(), anchorPc(),
                             out.lastPos);
            }
            return false;
        }
        if (nfTrace) {
            const ir::Insn* tn = dom.peek(order[out.lastPos]);
            std::fprintf(stderr, "[nf-gen] ok %s pc=%u lastPc=%u kind=%d\n",
                         std::string(s.backing().thisName()).c_str(), anchorPc(),
                         tn != nullptr ? tn->srcPc : 0u, static_cast<int>(out.value.kind));
        }
        return true;
    };

    ca::usize k = 0;
    while (k < in.size()) {
        const ir::Insn* ins = in[k];
        ValueLoad       a;
        ValueLoad       x;
        ValueLoad       y;
        if (ins->op == Op::GOTO) {
            // 空转跳两式：直落跳（目标 = 下一条实体指令）与跨死块跳（junk 在
            // goto 与目标之间塞 nop/athrow 死尾）。目标标签锚在窗内收集序上
            // 就跳过去继续走查，中间死指令不参与栈机（死代码无栈语义），改写
            // 期作为受害者一并 NOP 化（外部可达性由 genericLabelsOk 守门）。
            // 目标不在窗内（后跳/窗外）= 真跳转，词汇外走停。
            ca::usize k2 = static_cast<ca::usize>(-1);
            for (ca::usize j = k + 1; j < inIds.size(); ++j) {
                const auto labs = dom.labelsAt(inIds[j]);
                if (std::find(labs.begin(), labs.end(), ins->target) != labs.end()) {
                    k2 = j;
                    break;
                }
            }
            if (k2 == static_cast<ca::usize>(-1)) {
                return stopHere();
            }
            k = k2;   ///< 落到目标指令继续（死尾不进栈机）
            continue;
        }
        if (const_args::decodeValueLoad(s, *ins, a)) {
            stack.push_back(a);
        } else if (ins->op == Op::INVOKESTATIC) {
            // 包装调用三式（owner/name/desc 精确吻合；radix 恒 32）。
            if (isWrapperInvoke(s, *ins, "java/lang/Long", "parseUnsignedLong",
                                "(Ljava/lang/String;I)J")) {
                if (!popIf(ValueLoad::Kind::I32, x) || !popIf(ValueLoad::Kind::Str, y)
                    || x.i32() != 32) {
                    broken = true;   ///< radix 漂出家族生成面 = 走廊语义破缺
                    return stopHere();
                }
                ca::u64 keyBits = 0;
                if (!parseBase32(y.str, keyBits)) {
                    broken = true;   ///< 键面越出基 32 数字域
                    return stopHere();
                }
                stack.push_back(ValueLoad{ValueLoad::Kind::I64, keyBits, "", 2});
            } else if (isWrapperInvoke(s, *ins, "java/lang/Float", "intBitsToFloat",
                                       "(I)F")) {
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::F32,
                                          static_cast<ca::u64>(static_cast<ca::u32>(x.i32())),
                                          "", 1});
            } else if (isWrapperInvoke(s, *ins, "java/lang/Double", "longBitsToDouble",
                                       "(J)D")) {
                if (!popIf(ValueLoad::Kind::I64, x)) return stopHere();
                stack.push_back(
                    ValueLoad{ValueLoad::Kind::F64, static_cast<ca::u64>(x.i64()), "", 2});
            } else {
                return stopHere();
            }
        } else {
            switch (ins->op) {
            case Op::I2L:
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::I64,
                                          static_cast<ca::u64>(static_cast<ca::i64>(x.i32())),
                                          "", 2});
                break;
            case Op::I2F:
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(
                    ValueLoad{ValueLoad::Kind::F32,
                              rewrite_tools::bitCast<ca::u32>(static_cast<ca::f32>(x.i32())),
                              "", 1});
                break;
            case Op::I2D:
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::F64,
                                          rewrite_tools::bitCast<ca::u64>(
                                              static_cast<ca::f64>(x.i32())),
                                          "", 2});
                break;
            case Op::L2I:
                if (!popIf(ValueLoad::Kind::I64, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::I32,
                                          static_cast<ca::u64>(static_cast<ca::i64>(
                                              static_cast<ca::i32>(static_cast<ca::u32>(
                                                  x.i64())))),
                                          "", 1});
                break;
            case Op::L2F:
                if (!popIf(ValueLoad::Kind::I64, x)) return stopHere();
                stack.push_back(
                    ValueLoad{ValueLoad::Kind::F32,
                              rewrite_tools::bitCast<ca::u32>(static_cast<ca::f32>(
                                  static_cast<ca::i64>(x.i64()))),
                              "", 1});
                break;
            case Op::L2D:
                if (!popIf(ValueLoad::Kind::I64, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::F64,
                                          rewrite_tools::bitCast<ca::u64>(
                                              static_cast<ca::f64>(static_cast<ca::i64>(
                                                  x.i64()))),
                                          "", 2});
                break;
            case Op::F2D:
                if (!popIf(ValueLoad::Kind::F32, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::F64,
                                          rewrite_tools::bitCast<ca::u64>(
                                              static_cast<ca::f64>(x.f32())),
                                          "", 2});
                break;
            case Op::D2F:
                if (!popIf(ValueLoad::Kind::F64, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::F32,
                                          rewrite_tools::bitCast<ca::u32>(
                                              static_cast<ca::f32>(x.f64())),
                                          "", 1});
                break;
            case Op::I2B:
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::I32,
                                          static_cast<ca::u64>(static_cast<ca::i64>(
                                              static_cast<ca::i8>(static_cast<ca::u32>(
                                                  static_cast<ca::u32>(x.i32()))))),
                                          "", 1});
                break;
            case Op::I2C:
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(ValueLoad{
                    ValueLoad::Kind::I32,
                    static_cast<ca::u64>(static_cast<ca::u32>(x.i32()) & 0xFFFFu), "", 1});
                break;
            case Op::I2S:
                if (!popIf(ValueLoad::Kind::I32, x)) return stopHere();
                stack.push_back(ValueLoad{ValueLoad::Kind::I32,
                                          static_cast<ca::u64>(static_cast<ca::i64>(
                                              static_cast<ca::i16>(static_cast<ca::u16>(
                                                  static_cast<ca::u32>(x.i32()))))),
                                          "", 1});
                break;
            case Op::IXOR:
            case Op::IAND:
            case Op::IOR:
            case Op::IADD:
            case Op::ISUB:
            case Op::IMUL:
            case Op::ISHL:
            case Op::ISHR:
            case Op::IUSHR: {
                if (!popIf(ValueLoad::Kind::I32, y) || !popIf(ValueLoad::Kind::I32, x)) {
                    return stopHere();
                }
                const ca::u32 ua = static_cast<ca::u32>(x.i32());
                const ca::u32 ub = static_cast<ca::u32>(y.i32());
                ca::u32       r  = 0;
                switch (ins->op) {
                case Op::IXOR:  r = ua ^ ub; break;
                case Op::IAND:  r = ua & ub; break;
                case Op::IOR:   r = ua | ub; break;
                case Op::IADD:  r = ua + ub; break;
                case Op::ISUB:  r = ua - ub; break;
                case Op::IMUL:  r = ua * ub; break;
                case Op::ISHL:  r = ua << (ub & 31u); break;
                case Op::ISHR:  r = static_cast<ca::u32>(static_cast<ca::i32>(ua)
                                                         >> (ub & 31u));
                                break;
                case Op::IUSHR: r = ua >> (ub & 31u); break;
                default: return stopHere();
                }
                stack.push_back(ValueLoad{
                    ValueLoad::Kind::I32,
                    static_cast<ca::u64>(static_cast<ca::i64>(static_cast<ca::i32>(r))),
                    "", 1});
                break;
            }
            case Op::LXOR:
            case Op::LAND:
            case Op::LOR:
            case Op::LADD:
            case Op::LSUB:
            case Op::LMUL: {
                if (!popIf(ValueLoad::Kind::I64, y) || !popIf(ValueLoad::Kind::I64, x)) {
                    return stopHere();
                }
                const ca::u64 ua = static_cast<ca::u64>(x.i64());
                const ca::u64 ub = static_cast<ca::u64>(y.i64());
                ca::u64       r  = 0;
                switch (ins->op) {
                case Op::LXOR:  r = ua ^ ub; break;
                case Op::LAND:  r = ua & ub; break;
                case Op::LOR:   r = ua | ub; break;
                case Op::LADD:  r = ua + ub; break;
                case Op::LSUB:  r = ua - ub; break;
                case Op::LMUL:  r = ua * ub; break;
                default: return stopHere();
                }
                stack.push_back(ValueLoad{ValueLoad::Kind::I64, r, "", 2});
                break;
            }
            case Op::LSHL:
            case Op::LSHR:
            case Op::LUSHR: {
                // long 移位：先弹 I32 移位量，再弹 I64 被移值（JVM 栈序）。
                if (!popIf(ValueLoad::Kind::I32, y) || !popIf(ValueLoad::Kind::I64, x)) {
                    return stopHere();
                }
                const ca::u64 ua = static_cast<ca::u64>(x.i64());
                const ca::u64 ub = static_cast<ca::u64>(static_cast<ca::u32>(y.i32()));
                ca::u64       r  = 0;
                switch (ins->op) {
                case Op::LSHL:  r = ua << (ub & 63u); break;
                case Op::LSHR:  r = static_cast<ca::u64>(static_cast<ca::i64>(ua)
                                                         >> (ub & 63u));
                                break;
                case Op::LUSHR: r = ua >> (ub & 63u); break;
                default: return stopHere();
                }
                stack.push_back(ValueLoad{ValueLoad::Kind::I64, r, "", 2});
                break;
            }
            default:
                return stopHere();
            }
        }
        if (stack.size() == 1 && k >= 1) {
            closedTail  = k;
            closedValue = stack.back();
        }
        ++k;
    }
    return stopHere();
}

/// @brief 包装扩展：跨度尾随 Float.intBitsToFloat / Double.longBitsToDouble
///        时，跨度产出从整数值升成位形浮点常量（跨度延伸一条）。
///
/// 类型检查是语义事实：intBitsToFloat 只收 i32（int 跨度），longBitsToDouble
/// 只收 i64（long 跨度），引用吻合而类型不吻合即不扩展。
void extendWrapper(ir::ClassSession& s, const std::vector<ca::usize>& pos,
                   const std::vector<const ir::Insn*>& in, Span& out)
{
    const ca::usize wrapperIdx = out.tailIdx + 1;
    if (in.size() <= wrapperIdx) return;
    const ir::Insn* w = in[wrapperIdx];
    if (out.value.kind == ValueLoad::Kind::I32
        && isWrapperInvoke(s, *w, "java/lang/Float", "intBitsToFloat", "(I)F")) {
        out.lastPos    = pos[wrapperIdx];
        out.value.bits = static_cast<ca::u64>(static_cast<ca::u32>(out.value.i32()));
        out.value.kind = ValueLoad::Kind::F32;
        return;
    }
    if (out.value.kind == ValueLoad::Kind::I64
        && isWrapperInvoke(s, *w, "java/lang/Double", "longBitsToDouble", "(J)D")) {
        out.lastPos    = pos[wrapperIdx];
        out.value.bits = static_cast<ca::u64>(out.value.i64());
        out.value.kind = ValueLoad::Kind::F64;
    }
}

/// @brief 跨度内除锚点外的标签检查：任何中段指令带标签 = 合并点（外部到达
///        路径的栈形状与跨度产出耦合），站点放弃。
bool labelBlocked(const ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                  ca::usize firstPos, ca::usize lastPos)
{
    for (ca::usize p = firstPos + 1; p <= lastPos && p < order.size(); ++p) {
        if (!dom.labelsAt(order[p]).empty()) return true;
    }
    return false;
}

/// @brief 在 anchorPos 上钉跨度形状。锚点类型互斥分派（I32 / I64 / String）。
bool tryMatchAt(ir::ClassSession& s, const ir::CodeDom& dom,
                const std::vector<ir::InsnId>& order, ca::usize anchorPos, Span& span)
{
    std::vector<ca::usize>       pos;
    std::vector<const ir::Insn*> in;
    collectReal(dom, order, anchorPos, pos, in);
    if (in.size() < 2) return false;

    // 锚点必须是常量装载（数值或字符串键）；fconst/dconst、null、Class 等
    // 引用形态不是任何跨度的起点。
    ValueLoad anchor;
    if (!const_args::decodeValueLoad(s, *in[0], anchor)) return false;
    if (anchor.kind != ValueLoad::Kind::I32 && anchor.kind != ValueLoad::Kind::I64
        && anchor.kind != ValueLoad::Kind::Str) {
        return false;
    }

    Span out;
    out.value = anchor;
    bool matched = false;
    if (anchor.kind == ValueLoad::Kind::I32) {
        // 旧式先试：新式只接旧式拒绝的站点（旧四式折叠数不受新式影响）。
        matched = matchIntViaLong(s, pos, in, out) || matchIntSplice(s, pos, in, out);
    } else if (anchor.kind == ValueLoad::Kind::I64) {
        matched = matchLongToInt(s, pos, in, out) || matchLongSplice(s, pos, in, out);
    } else if (anchor.kind == ValueLoad::Kind::Str) {
        matched = matchBase32Long(s, pos, in, out);
    }
    if (!matched) {
        // 通用封闭跨度兜底：拼装走廊族的最长封闭前缀（栈机求值）。包装调用
        // 在走查词汇表内消费，无需 extendWrapper；标签走 genericLabelsOk
        // 普查口径（空转 goto 自引用不算合并点），已收编进匹配器本体。
        matched = matchGenericSpan(s, dom, order, anchorPos, out);
        if (!matched) return false;
        out.nopVictims = true;   ///< 改写走 NOP 化口径（Span::nopVictims 注）
        span = out;
        return true;
    }
    extendWrapper(s, pos, in, out);
    // 命名形状：严格合并点口径（跨度内除锚点外任何标签都拒绝）。
    if (labelBlocked(dom, order, out.firstPos, out.lastPos)) return false;
    span = out;
    return true;
}

/// @brief ValueLoad → SimValue（makeConstLoad 的输入面）。
jsim::SimValue simOf(const ValueLoad& v)
{
    switch (v.kind) {
    case ValueLoad::Kind::I32: return jsim::SimValue::knownI32(v.i32());
    case ValueLoad::Kind::I64: return jsim::SimValue::knownI64(v.i64());
    case ValueLoad::Kind::F32: return jsim::SimValue::knownF32(v.f32());
    case ValueLoad::Kind::F64: return jsim::SimValue::knownF64(v.f64());
    default: return jsim::SimValue{};
    }
}

/// @brief 单方法 SCAN。@return false = 类级迭代预算超限（调用方放弃该类剩余方法）。
bool scanMethod(ir::ClassSession& s, ca::usize methodIndex, ca::u32 classIdx,
                ScanResult& result, iter_budget::ClassBudget& budget)
{
    if (!ir::hasCode(s, methodIndex)) return true;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return true;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return true;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      pos   = 0;
    while (pos < order.size()) {
        if (!budget.tick()) return false;   ///< 逐位置计数：超限即停，类保守跳过
        const ir::Insn* in = cu->dom.peek(order[pos]);
        if (in == nullptr || in->op == Op::NOP) {
            ++pos;
            continue;
        }
        Span span;
        if (tryMatchAt(s, cu->dom, order, pos, span)) {
            // 折叠值收进 POD 位形：匹配成功后 kind 恒为数值四形（基 32 键已就地折叠并清串），跨段数据流只留 kind + bits。
            result.sites.push_back(Site{classIdx, methodIndex, in->srcPc, in->op,
                                        SiteValue{span.value.kind, span.value.bits}});
            pos = span.lastPos + 1;   ///< 跨度内部不再起锚（锚点类型互斥，无嵌套站点）
            continue;
        }
        ++pos;
    }
    return true;
}

/// @brief SMT 帧的 delta 重编：压缩 same/same1 形的 delta 编在 type 字节里，
///        超出 63 的 delta 升格为显式 offset 形（JVMS 4.7.4 的
///        same_frame_extended / same_locals_1_stack_item_extended）。
/// @return false = 该帧的 delta 落在压缩形编不下的区间（升格形不写，宁剪
///         失败整站放弃，不产字节面歧义）。
bool smtSetDelta(jbc::core::StackMapTableAttr::Frame& f, ca::u32 delta)
{
    using Frame = jbc::core::StackMapTableAttr::Frame;
    if (f.type <= Frame::kSameMax) {
        if (delta <= Frame::kSameMax) {
            f.type = static_cast<ca::u8>(delta);
            return true;
        }
        return false;
    }
    if (f.type >= Frame::kSame1Base && f.type <= Frame::kSame1Max) {
        if (delta <= Frame::kSameMax) {
            f.type = static_cast<ca::u8>(Frame::kSame1Base + delta);
            return true;
        }
        return false;
    }
    f.offset = delta;   ///< 247..255：显式 delta 族，offset 直写
    return true;
}

bool pcInList(const std::vector<ca::u32>& list, ca::u32 pc)
{
    return std::find(list.begin(), list.end(), pc) != list.end();
}

/// @brief 剪掉锚在受害者 pc 上的 SMT 分支目标帧（NOP 化口径的配套面）。
///
/// 走廊里的透明 goto 是中间态帧的唯一引用者：goto 随 NOP 化消亡后，其目标
/// pc 上的帧不再被任何分支引用，但 lower 会按 pcmap 忠实搬移——搬过去就是
/// 一张描述已消失栈形的 stale 帧，verifier 直接 VerifyError（b.clinit 实证：
/// 帧要求 stack[0]=String，实际只剩 anchor 的 int）。本入口在 NOP 化之前把
/// 这些死帧从 backing 的 StackMapTable 属性里剪掉（幸存帧 delta 链同步重算），
/// exc handler 落点例外保留（异常分派 JVM 规范先清栈，帧内容 [exc] 与走廊
/// 中间态无关，NOP 保 pc 后落点不变）。前置：方法已 promote（Code 属性解码
/// 已趟过一遍）。@return false = SMT 存在但判读不了（解码失败 / 保留标签
/// 128..246），调用方整站放弃，宁不折不炸。
bool pruneSmtFrames(jbc::core::ClassFile& cf, jbc::core::MemberInfo& member,
                    const std::vector<ca::u32>& victimPcs,
                    const std::vector<ca::u32>& handlerPcs)
{
    using Frame = jbc::core::StackMapTableAttr::Frame;
    auto codeR  = cf.decodeCode(member);
    if (codeR.is_err()) return false;
    jbc::core::CodeAttr* code = codeR.unwrap();
    if (code == nullptr) return true;   ///< 无 Code = 无帧面
    for (jbc::core::AttrSlot& slot : code->attrs) {
        if (cf.utf8(slot.nameCp) != "StackMapTable") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        if (d.unwrap() == nullptr
            || d.unwrap()->kind() != jbc::core::AttrKind::StackMapTable) {
            return false;
        }
        auto* body = slot.mutate();   ///< 置 Modified：写回走重编码
        if (body == nullptr) return false;
        auto* smt = static_cast<jbc::core::StackMapTableAttr*>(body);

        std::vector<ca::u32> abs(smt->frames.size());
        ca::u32              prev = static_cast<ca::u32>(-1);   ///< 首帧 delta 即 pc
        for (ca::usize i = 0; i < smt->frames.size(); ++i) {
            const Frame& f = smt->frames[i];
            ca::u32 delta = 0;
            if (f.type <= Frame::kSameMax) {
                delta = f.type;
            } else if (f.type >= Frame::kSame1Base && f.type <= Frame::kSame1Max) {
                delta = f.type - Frame::kSame1Base;
            } else if (f.type == Frame::kSame1Ext
                       || (f.type >= Frame::kChopMax && f.type <= Frame::kFull)) {
                delta = f.offset;
            } else {
                return false;   ///< 128..246 保留标签：不猜
            }
            abs[i] = prev + delta + 1;
            prev   = abs[i];
        }

        std::vector<Frame> kept;
        kept.reserve(smt->frames.size());
        prev = static_cast<ca::u32>(-1);
        for (ca::usize i = 0; i < smt->frames.size(); ++i) {
            const ca::u32 pc = abs[i];
            if (pcInList(victimPcs, pc) && !pcInList(handlerPcs, pc)) continue;
            Frame f = smt->frames[i];
            if (!smtSetDelta(f, pc - prev - 1)) {   ///< u32 回绕：首帧 prev=-1 时恰为 pc
                return false;   ///< 升格形不写：整站放弃
            }
            kept.push_back(std::move(f));
            prev = pc;
        }
        smt->frames = std::move(kept);
        if (std::getenv("GRUNT_NF_SMTTRACE") != nullptr) {
            ca::u32       chk = static_cast<ca::u32>(-1);
            const ca::u32 w   = static_cast<ca::u32>(-1);
            for (const Frame& f : smt->frames) {
                ca::u32 d = 0;
                if (f.type <= Frame::kSameMax) {
                    d = f.type;
                } else if (f.type >= Frame::kSame1Base && f.type <= Frame::kSame1Max) {
                    d = f.type - Frame::kSame1Base;
                } else {
                    d = f.offset;
                }
                chk = chk + d + 1;
                std::fprintf(stderr, "[nf-smt] type=%u delta=%u abs=%u (prev=%u)\n",
                             static_cast<ca::u32>(f.type), d, chk,
                             chk >= d + 1 ? chk - d - 1 : w);
            }
        }
        return true;   ///< 单方法至多一张 SMT，剪完即返
    }
    return true;   ///< 无 SMT = 无帧面
}

/// @brief 剪掉锚在受害者 pc 上的 LNT/LVT 行（擦除口径的配套面）。
///
/// 擦除让受害者旧 pc 从 pcmap 蒸发，行锚 miss 即 Err(RelocationBadTarget)
/// （B#5/c#3 实证：命名形状站点擦除后整方法 lower 失败连坐）。行是调试面，
/// 删行无损语义；LVT 行的终点落在受害者上同样 miss，一并剪。前置：方法已
/// promote。@return false = 属性判读不了，调用方整站放弃。
bool prunePcAttrs(jbc::core::ClassFile& cf, jbc::core::MemberInfo& member,
                  const std::vector<ca::u32>& victimPcs)
{
    auto codeR = cf.decodeCode(member);
    if (codeR.is_err()) return false;
    jbc::core::CodeAttr* code = codeR.unwrap();
    if (code == nullptr) return true;   ///< 无 Code = 无行面
    for (jbc::core::AttrSlot& slot : code->attrs) {
        const std::string_view n = cf.utf8(slot.nameCp);
        const bool isLnt = n == "LineNumberTable";
        const bool isLvt = n == "LocalVariableTable" || n == "LocalVariableTypeTable";
        if (!isLnt && !isLvt) continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        if (d.unwrap() == nullptr) return false;
        const auto kind = d.unwrap()->kind();
        if (kind != jbc::core::AttrKind::LineNumberTable
            && kind != jbc::core::AttrKind::LocalVariableTable) {
            return false;
        }
        auto* body = slot.mutate();   ///< 置 Modified：写回走重编码
        if (body == nullptr) return false;
        if (isLnt) {
            auto& lnt = static_cast<jbc::core::LineNumberTableAttr&>(*body);
            std::vector<jbc::core::LineNumberTableAttr::Row> kept;
            kept.reserve(lnt.rows.size());
            for (auto& r : lnt.rows) {
                if (pcInList(victimPcs, r.startPc)) continue;
                kept.push_back(r);
            }
            lnt.rows = std::move(kept);
        } else {
            auto& lvt = static_cast<jbc::core::LocalVariableTableAttr&>(*body);
            std::vector<jbc::core::LocalVariableTableAttr::Row> kept;
            kept.reserve(lvt.rows.size());
            for (auto& r : lvt.rows) {
                if (pcInList(victimPcs, r.startPc)
                    || pcInList(victimPcs, r.startPc + r.length)) {
                    continue;
                }
                kept.push_back(r);
            }
            lvt.rows = std::move(kept);
        }
    }
    return true;
}

/// @brief 单方法 REWRITE。@return 折叠成功的站点数；failed 走出参。
///
/// cu 由 promoteMethod 提升好。站点按 srcPc+op
/// 重定位后全量复核（scan 之后字节可能已被同方法前站改写，不盲改），锚点
/// 原地改成结果常量装载，跨度其余非 NOP 指令擦除。
/// budget 逐站点按方法指令数计数（重定位是线性扫）；超限时剩余站点保守放弃。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ca::usize& failed,
                        ca::usize& skippedByBudget, iter_budget::ClassBudget& budget)
{
    const std::vector<ir::InsnId>& order = cu->dom.order();
    ca::usize                      hit   = 0;
    ca::usize                      tried = 0;   ///< 本方法已尝试的站点数

    for (const Site* site : sites) {
        // 逐站点按方法指令数计数：重定位 + 复核都是对 order 的线性扫。
        if (!budget.tick(order.size())) {
            skippedByBudget += sites.size() - tried;   ///< 余下站点保守放弃
            return hit;
        }
        ++tried;
        // 重定位：srcPc + op 跨会话稳定（InsnId 不带出去）。
        ca::usize anchorPos = static_cast<ca::usize>(-1);
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const ir::Insn* in = cu->dom.peek(order[pos]);
            if (in != nullptr && in->srcPc == site->anchorSrcPc && in->op == site->anchorOp) {
                anchorPos = pos;
                break;
            }
        }
        if (anchorPos == static_cast<ca::usize>(-1)) {
            if (std::getenv("GRUNT_NF_TRACE") != nullptr) {
                std::fprintf(stderr, "[nf-rewrite] fail relocate %s#%zu pc=%u op=%d\n",
                             std::string(s.backing().thisName()).c_str(),
                             site->methodIndex, site->anchorSrcPc,
                             static_cast<int>(site->anchorOp));
            }
            ++failed;
            continue;
        }

        // 跨度复核：形状与值都要对上（前站改写后不盲改）；标签检查已收编进
        // tryMatchAt（scan 同一判定，防同方法前站改写引入新边界）。
        Span span;
        const bool nfTrace = std::getenv("GRUNT_NF_TRACE") != nullptr;
        if (!tryMatchAt(s, cu->dom, order, anchorPos, span)
            || span.firstPos != anchorPos
            || span.value.kind != site->value.kind || span.value.bits != site->value.bits) {
            if (nfTrace) {
                std::fprintf(stderr, "[nf-rewrite] fail recheck %s#%zu pc=%u\n",
                             std::string(s.backing().thisName()).c_str(),
                             site->methodIndex, site->anchorSrcPc);
            }
            ++failed;
            continue;
        }
        if (nfTrace) {
            std::fprintf(stderr, "[nf-site] %s#%zu pc=%u generic=%d\n",
                         std::string(s.backing().thisName()).c_str(),
                         site->methodIndex, site->anchorSrcPc,
                         span.nopVictims ? 1 : 0);
        }

        auto slotR = cu->dom.get(order[anchorPos]);
        if (slotR.is_err()) {
            ++failed;
            continue;
        }
        ir::Insn*     anchor   = slotR.unwrap();
        const ir::Insn original = *anchor;

        ir::Insn fresh{};
        if (!rewrite_tools::makeConstLoad(s, simOf(span.value), fresh)) {
            if (nfTrace) {
                std::fprintf(stderr, "[nf-rewrite] fail constload %s#%zu pc=%u\n",
                             std::string(s.backing().thisName()).c_str(),
                             site->methodIndex, site->anchorSrcPc);
            }
            ++failed;
            continue;
        }
        fresh.srcPc = original.srcPc;   ///< 锚点保 srcPc：下游 pcmap/调试行落点不变
        *anchor     = fresh;

        // 跨度其余非 NOP 指令出列。NOP 是 squeeze 遗留落点，留着无害（DeadCode
        // 收尾）。两条改写口径：
        //   - 通用跨度（nopVictims）：受害者 NOP 化保 srcPc——透明 goto 的目标
        //     ControlFlow 标签锚在跨度内部成员上，擦除会让标签悬垂，lower 的
        //     pcmap 重定位报 relocation bad target（拼装走廊实证）；NOP 化后
        //     SMT/LNT/异常表原地落点不变，语义恒等（透明 goto 本就是落到下条
        //     实体指令的空转，NOP 同为顺序执行）。异常分派 JVM 规范先清栈，
        //     handler 帧只含 [exc]，与走廊中间栈形无关，无需护栏。
        //   - 命名形状：eraseBatch 照旧。kRejectIfUsed：受害者被异常表引用时
        //     拒收，回滚锚点，整站放弃，不留常量已覆盖、跨度还在的半改写态。
        std::vector<ir::InsnId> victims;
        for (ca::usize p = anchorPos + 1; p <= span.lastPos && p < order.size(); ++p) {
            const ir::Insn* victim = cu->dom.peek(order[p]);
            if (victim == nullptr || victim->op == Op::NOP) continue;
            victims.push_back(order[p]);
        }
        // 真实受害者旧 pc 集（两条口径共用：NOP 化对象 / 擦除对象）。
        std::vector<ca::u32> victimPcs;
        victimPcs.reserve(victims.size());
        for (const ir::InsnId vid : victims) {
            const ir::Insn* v = cu->dom.peek(vid);
            if (v != nullptr) victimPcs.push_back(v->srcPc);
        }
        if (span.nopVictims) {
            // 跨度内部全 pc（含既有 NOP：obfuscator squeeze 遗留也常是分支
            // 目标、也带帧，剪枝面必须盖住它们，否则 stale 帧残存 →
            // VerifyError b.b 实证）。
            std::vector<ca::u32> spanPcs;
            spanPcs.reserve(span.lastPos - anchorPos);
            for (ca::usize p = anchorPos + 1; p <= span.lastPos && p < order.size(); ++p) {
                const ir::Insn* v = cu->dom.peek(order[p]);
                if (v != nullptr) spanPcs.push_back(v->srcPc);
            }
            std::vector<ca::u32> handlerPcs;
            for (const auto& e : cu->exc) {
                auto hr = cu->dom.resolveLabel(e.handler);
                if (hr.is_err() || hr.unwrap().isEnd()) continue;
                const ir::Insn* h = cu->dom.peek(hr.unwrap().insn);
                if (h != nullptr) handlerPcs.push_back(h->srcPc);
            }
            // 异常 handler 落点在受害者上 = handler 体就在跨度里（junk 死尾
            // 场景），NOP 化会毁掉 handler 逻辑——整站放弃（擦除路径的
            // kRejectIfUsed 同款口径）。纯范围边界（start/end）不受害：pc 全
            // 保留，try 区间覆盖面不变。
            bool handlerHit = false;
            for (const ca::u32 hp : handlerPcs) {
                if (pcInList(victimPcs, hp)) {
                    handlerHit = true;
                    break;
                }
            }
            if (handlerHit) {
                *anchor = original;
                if (nfTrace) {
                    std::fprintf(stderr, "[nf-rewrite] fail exchandler %s#%zu pc=%u\n",
                                 std::string(s.backing().thisName()).c_str(),
                                 site->methodIndex, site->anchorSrcPc);
                }
                ++failed;
                continue;
            }
            const ir::ClassUnit& unit = s.unit();
            const bool canPrune =
                site->methodIndex < unit.methods.size()
                && unit.methods[site->methodIndex].backingIndex < s.backing().methods.size();
            if (!canPrune
                || !pruneSmtFrames(s.backing(),
                                   s.backing().methods[unit.methods[site->methodIndex]
                                                           .backingIndex],
                                   spanPcs, handlerPcs)) {
                *anchor = original;
                if (nfTrace) {
                    std::fprintf(stderr, "[nf-rewrite] fail smtprune %s#%zu pc=%u\n",
                                 std::string(s.backing().thisName()).c_str(),
                                 site->methodIndex, site->anchorSrcPc);
                }
                ++failed;
                continue;
            }
            // 先取齐可变槽位再统一写：get 只在句柄无效时失败，order 里刚 peek
            // 过的句柄必有效，但两段式让失败面天然不落半改写态。
            std::vector<ir::Insn*> slots;
            slots.reserve(victims.size());
            bool slotErr = false;
            for (const ir::InsnId vid : victims) {
                auto vr = cu->dom.get(vid);
                if (vr.is_err()) {
                    slotErr = true;
                    break;
                }
                slots.push_back(vr.unwrap());
            }
            if (slotErr) {
                *anchor = original;
                if (nfTrace) {
                    std::fprintf(stderr, "[nf-rewrite] fail nopslot %s#%zu pc=%u\n",
                                 std::string(s.backing().thisName()).c_str(),
                                 site->methodIndex, site->anchorSrcPc);
                }
                ++failed;
                continue;
            }
            for (ir::Insn* vm : slots) {
                const ca::u32 keepPc = vm->srcPc;
                *vm       = ir::Insn{};   ///< 清 target/targets：透明 goto 的标签引用随写消失
                vm->op    = Op::NOP;
                vm->srcPc = keepPc;
            }
        } else if (!victims.empty()) {
            // 擦除口径：受害者旧 pc 即将从 pcmap 蒸发，LNT/LVT 行锚 miss 即
            // lower Err——先剪行（调试面删行无损），再 eraseBatch。SMT 帧不会
            // 锚在擦除受害者上：labelBlocked 已把带 DOM 标签（含分支目标）的
            // 受害者整站拒掉。
            const ir::ClassUnit& unit = s.unit();
            if (!prunePcAttrs(s.backing(),
                              s.backing().methods[unit.methods[site->methodIndex]
                                                      .backingIndex],
                              victimPcs)) {
                *anchor = original;
                if (nfTrace) {
                    std::fprintf(stderr, "[nf-rewrite] fail pcattrs %s#%zu pc=%u\n",
                                 std::string(s.backing().thisName()).c_str(),
                                 site->methodIndex, site->anchorSrcPc);
                }
                ++failed;
                continue;
            }
            if (cu->dom.eraseBatch(victims).is_err()) {
                *anchor = original;
                if (nfTrace) {
                    std::fprintf(stderr, "[nf-rewrite] fail erase %s#%zu pc=%u victims=%zu\n",
                                 std::string(s.backing().thisName()).c_str(),
                                 site->methodIndex, site->anchorSrcPc, victims.size());
                }
                ++failed;
                continue;
            }
        }
        ++hit;
    }
    return hit;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budgetTemplate)
{
    ScanResult result;
    ca::usize  named = 0;
    ca::u32    classIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = classIdx++;   ///< 类序号与 rewrite 的遍历同序（同一 visitResources 序）
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置计数
        ir::ClassSession&        s      = *session;
        const ir::ClassUnit&     unit   = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!scanMethod(s, mi, idx, result, budget)) {
                ++result.classesSkippedByBudget;
                iter_budget::reportExhausted("grunt.NumberFold", "scan",
                                             std::string(entry.uri), budget, named);
                return;   ///< 该类剩余方法不再扫（保守跳过）
            }
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budgetTemplate)
{
    DirtySet dirty;

    // 站点按类序号聚类（一遍建索引；此前逐类线性筛全站点的做法在万类大 jar 上
    // 是 classes × sites 的平方账）。scan/rewrite 两段对同一容器走同一
    // visitResources 序，classIdx 跨段一致。
    std::unordered_map<ca::u32, std::vector<const Site*>> byIdx;
    for (const Site& site : result.sites) byIdx[site.classIdx].push_back(&site);

    ca::usize named  = 0;
    ca::u32   curIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = curIdx++;
        const auto    it  = byIdx.find(idx);
        if (it == byIdx.end()) return;
        // 先值拷出再删节点：map 节点销毁会连带销毁聚类向量，引用不能跨 erase。
        const std::vector<const Site*> sitesOf = it->second;
        byIdx.erase(it);   ///< 该类站点已到手，聚类条目即弃（峰值驻留只留当前类）

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置计数

        // 前置检查：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit             = 0;
        ca::usize failed          = 0;
        ca::usize skippedByBudget = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            dirty.attempted += sitesOfMethod.size();
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) {
                if (std::getenv("GRUNT_NF_TRACE") != nullptr) {
                    std::fprintf(stderr, "[nf-rewrite] fail promote %s#%zu sites=%zu\n",
                                 std::string(s.backing().thisName()).c_str(), mi,
                                 sitesOfMethod.size());
                }
                failed += sitesOfMethod.size();
                continue;
            }
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) {
                if (std::getenv("GRUNT_NF_TRACE") != nullptr) {
                    std::fprintf(stderr, "[nf-rewrite] fail promote-null %s#%zu\n",
                                 std::string(s.backing().thisName()).c_str(), mi);
                }
                failed += sitesOfMethod.size();
                continue;
            }
            const ca::usize methodHit =
                rewriteMethod(s, cu, sitesOfMethod, failed, skippedByBudget, budget);
            if (methodHit == 0) continue;
            // 保真写回：lower 按 pcmap 重定位 SMT/LVT/LNT，帧内容不动。折叠是
            // 栈形等价的语义恒等替换，无需帧重推导——Strict 帧再生的 CFG 推
            // 导对 junk 谓词走廊方法会把 append 帧的 locals 基算丢（Grunt 样
            // 本实证 B.c/u.b VerifyError，池类 clinit 求值因此 Bail 连锁破坏
            // 下游证据面），语义恒等替换不走推导（pool_array_fold 同一修复）。
            if (std::getenv("GRUNT_NF_PCCHK") != nullptr) {
                // lower 前穷举探针：所有会按 pcmap 重定向的属性锚，逐一对照
                // 幸存指令的旧 pc 集，miss 即打印（定位 RelocationBadTarget）。
                std::vector<ca::u32> alive;
                for (const ir::InsnId id : cu->dom.order()) {
                    const ir::Insn* v = cu->dom.peek(id);
                    if (v != nullptr) alive.push_back(v->srcPc);
                }
                jbc::core::MemberInfo& mem =
                    s.backing().methods[s.unit().methods[mi].backingIndex];
                auto codeR2 = s.backing().decodeCode(mem);
                if (codeR2.is_ok() && codeR2.unwrap() != nullptr) {
                    // 旧 code_length：LVT 行终点 == 旧 code_length 是 lower 的
                    // 合法面（整方法 local 行），探针不计 miss。
                    const ca::u32 oldLen =
                        static_cast<ca::u32>(codeR2.unwrap()->code.size());
                    for (jbc::core::AttrSlot& sl2 : codeR2.unwrap()->attrs) {
                        const std::string_view n2 = s.backing().utf8(sl2.nameCp);
                        auto d2 = s.backing().decodeAttr(sl2);
                        if (d2.is_err() || d2.unwrap() == nullptr) continue;
                        const std::string_view tag =
                            n2 == "StackMapTable"
                                ? "smt"
                                : (n2 == "LineNumberTable"
                                       ? "lnt"
                                       : (n2 == "LocalVariableTable"
                                              ? "lvt"
                                              : (n2 == "LocalVariableTypeTable"
                                                     ? "lvtt"
                                                     : (n2 == "RuntimeVisibleTypeAnnotations"
                                                            || n2 == "RuntimeInvisibleTypeAnnotations"
                                                            ? "ta"
                                                            : ""))));
                        if (tag.empty()) continue;
                        if (tag == "ta") {
                            using TA = jbc::core::TypeAnnotationsAttr;
                            auto& ta = static_cast<TA&>(*d2.unwrap());
                            ca::usize idx = 0;
                            for (const auto& ann : ta.annotations) {
                                const ca::u8 tt = ann.targetType;
                                if (tt >= TA::kOffsetBase && tt <= TA::kTypeArgMax) {
                                    if (!pcInList(alive, ann.offset)) {
                                        std::fprintf(stderr,
                                                     "[nf-pcchk] %s#%zu ta[%zu] tt=%02x offset=%u MISS\n",
                                                     std::string(s.backing().thisName())
                                                         .c_str(),
                                                     mi, idx, static_cast<ca::u32>(tt),
                                                     ann.offset);
                                    }
                                } else if (tt == TA::kLocalVar
                                           || tt == TA::kLocalVarResource) {
                                    for (const auto& e : ann.localVars) {
                                        const bool endOk = pcInList(alive, e.startPc + e.length)
                                                           || e.startPc + e.length == oldLen;
                                        if (!pcInList(alive, e.startPc) || !endOk) {
                                            std::fprintf(stderr,
                                                         "[nf-pcchk] %s#%zu ta[%zu] localvar start=%u end=%u MISS\n",
                                                         std::string(s.backing().thisName())
                                                             .c_str(),
                                                         mi, idx, e.startPc,
                                                         e.startPc + e.length);
                                        }
                                    }
                                }
                                ++idx;
                            }
                        } else if (tag == "smt") {
                            auto& sm = static_cast<jbc::core::StackMapTableAttr&>(*d2.unwrap());
                            ca::u32 pv = static_cast<ca::u32>(-1);
                            for (const auto& fr : sm.frames) {
                                ca::u32 dl = 0;
                                if (fr.type <= jbc::core::StackMapTableAttr::Frame::kSameMax) {
                                    dl = fr.type;
                                } else if (fr.type
                                               >= jbc::core::StackMapTableAttr::Frame::kSame1Base
                                           && fr.type
                                               <= jbc::core::StackMapTableAttr::Frame::kSame1Max) {
                                    dl = fr.type - jbc::core::StackMapTableAttr::Frame::kSame1Base;
                                } else {
                                    dl = fr.offset;
                                }
                                pv = pv + dl + 1;
                                if (!pcInList(alive, pv)) {
                                    std::fprintf(stderr, "[nf-pcchk] %s#%zu smt abs=%u MISS\n",
                                                 std::string(s.backing().thisName()).c_str(),
                                                 mi, pv);
                                }
                            }
                        } else if (tag == "lnt") {
                            auto& ln = static_cast<jbc::core::LineNumberTableAttr&>(*d2.unwrap());
                            for (const auto& r : ln.rows) {
                                if (!pcInList(alive, r.startPc)) {
                                    std::fprintf(stderr, "[nf-pcchk] %s#%zu lnt start=%u MISS\n",
                                                 std::string(s.backing().thisName()).c_str(),
                                                 mi, r.startPc);
                                }
                            }
                        } else {
                            auto& lv = static_cast<jbc::core::LocalVariableTableAttr&>(*d2.unwrap());
                            for (const auto& r : lv.rows) {
                                const bool endOk = pcInList(alive, r.startPc + r.length)
                                                   || r.startPc + r.length == oldLen;
                                if (!pcInList(alive, r.startPc) || !endOk) {
                                    std::fprintf(stderr,
                                                 "[nf-pcchk] %s#%zu %s start=%u end=%u MISS\n",
                                                 std::string(s.backing().thisName()).c_str(),
                                                 mi, tag == "lvt" ? "lvt" : "lvtt",
                                                 r.startPc, r.startPc + r.length);
                                }
                            }
                        }
                    }
                }
            }
            auto lowered = ir::lowerMethod(s, mi);
            if (lowered.is_err()) {
                // 写回失败：该方法整方法放弃（session 内半改写态不外泄）。
                if (std::getenv("GRUNT_NF_TRACE") != nullptr) {
                    std::fprintf(stderr, "[nf-rewrite] fail lower %s#%zu err=%s sites=",
                                 std::string(s.backing().thisName()).c_str(), mi,
                                 ir::to_string(lowered.unwrap_err()));
                    for (const Site* sp : sitesOfMethod) {
                        std::fprintf(stderr, "%u ", sp->anchorSrcPc);
                    }
                    std::fprintf(stderr, "\n");
                }
                failed += methodHit;
                continue;
            }
            hit += methodHit;
        }
        dirty.failed += failed;
        dirty.skippedByBudget += skippedByBudget;
        if (skippedByBudget > 0) {
            iter_budget::reportExhausted("grunt.NumberFold", "rewrite",
                                         std::string(entry.uri), budget, named);
        }
        if (hit == 0) return;

        // 类字节就地回写：类资源内容原位换新（容器映射不动、无待写缓冲，
        // 万类大 jar 上全量 dirty 类字节驻留是 GB 级账，流式写回把峰值削平）。
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        mjbase::model::IResource* raw = container.getResource(entry.uri);
        if (!ca::core::tag_cast::isa<mjbase::model::ClassResource>(raw)) return;
        auto* clsRes = ca::core::tag_cast::cast<mjbase::model::ClassResource>(raw);
        if (clsRes == nullptr) return;
        clsRes->set(std::move(encoded).unwrap());
        dirty.uris.emplace_back(entry.uri);
        dirty.sites += hit;
    });
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::number_fold
