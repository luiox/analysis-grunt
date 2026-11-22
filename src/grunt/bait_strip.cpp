/// @file        bait_strip.cpp
/// @brief       残留 `bait_strip` 的三段实现：SCAN / REWRITE / PROOF。
///
/// 判据等级：S2 结构事实。诱饵判据按 Utf8 内容 contains（`<thinking>` /
/// `signal="automated decompiler transcript"` 两标记，普查脚本
/// verify_bait 实证 128 distinct 全部落此家族）；站点可达性按字节级走查
/// （junk_region_strip 同款 opcode 长度表 + worklist）。孤儿普查是效力
/// 检查不是安全防线——安全由 libjbc cp_compact 的引用闭包证明兜底（见
/// bait_strip.hpp 改写一节）。
#include "grunt/bait_strip.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <utility>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/pass_clock.hpp"

namespace grunt::bait_strip {

/// @brief 诱饵内容判据：字节级 contains 任一家族标记。
///
/// 两标记对应普查的两个口径：`<thinking>` 是全家族前缀（222/30 与 248/42
/// 的分子），全串（`signal="automated decompiler transcript"`）是 128
/// distinct 里的 8 条满配变体。按字节搜，不管 modified UTF-8 解码。
bool isBaitContent(std::string_view content)
{
    constexpr std::string_view kThinking
        = "<thinking>";
    constexpr std::string_view kTranscript
        = "signal=\"automated decompiler transcript\"";
    return content.find(kThinking) != std::string_view::npos
           || content.find(kTranscript) != std::string_view::npos;
}

namespace {

namespace ir = jbc::ir;

constexpr ca::u8 kNop    = 0x00;
constexpr ca::u8 kOpLdc  = 0x12;
constexpr ca::u8 kOpLdcW = 0x13;

constexpr ca::usize kNpos = static_cast<ca::usize>(-1);

// ============================================================================
// ==== 可达性走查（junk_region_strip 同款手法，复制进本文件）===================
// ============================================================================

/// @brief 单操作码长度表（字节含操作码本身）；表外操作码返回 0 = fail-closed。
///        完整 JVM 表（JVMS 6.5）；变长族（switch/wide）由走查器特判。
ca::u8 insnLength(ca::u8 op) noexcept
{
    if (op <= 0x0f) return 1;                       // nop..dconst_1
    if (op == 0x10 || op == 0x12) return 2;         // bipush / ldc
    if (op == 0x11 || op == 0x13 || op == 0x14) return 3;  // sipush / ldc_w / ldc2_w
    if (op >= 0x15 && op <= 0x19) return 2;         // xload
    if (op >= 0x1a && op <= 0x2d) return 1;         // xload_n
    if (op >= 0x2e && op <= 0x35) return 1;         // xaload
    if (op >= 0x36 && op <= 0x3a) return 2;         // xstore
    if (op >= 0x3b && op <= 0x4e) return 1;         // xstore_n
    if (op >= 0x4f && op <= 0x56) return 1;         // xastore
    if (op >= 0x57 && op <= 0x83) return 1;         // pop..lxor
    if (op == 0x84) return 3;                       // iinc
    if (op >= 0x85 && op <= 0x98) return 1;         // i2l..dcmpg
    if (op >= 0x99 && op <= 0xa4) return 3;         // ifeq..if_icmple
    if (op == 0xa5 || op == 0xa6) return 3;         // if_acmpeq/ne
    if (op == 0xa7 || op == 0xa8 || op == 0xa9) return 3;  // goto / jsr / ret
    if (op == 0xaa || op == 0xab) return 0;         // switch：变长，特判
    if (op >= 0xac && op <= 0xb1) return 1;         // xreturn
    if (op >= 0xb2 && op <= 0xb8) return 3;         // getstatic..invokestatic
    if (op == 0xb9 || op == 0xba) return 5;         // invokeinterface / invokedynamic
    if (op == 0xbb) return 3;                       // new
    if (op == 0xbc) return 2;                       // newarray
    if (op == 0xbd || op == 0xc0 || op == 0xc1) return 3;  // anewarray / checkcast / instanceof
    if (op >= 0xbe && op <= 0xbf) return 1;         // arraylength / athrow
    if (op >= 0xc2 && op <= 0xc3) return 1;         // monitor
    if (op == 0xc4) return 0;                       // wide：特判
    if (op == 0xc5) return 4;                       // multianewarray
    if (op == 0xc6 || op == 0xc7) return 3;         // ifnull / ifnonnull
    if (op == 0xc8 || op == 0xc9) return 5;         // goto_w / jsr_w
    return 0;                                       // 0xca+ 表外：fail-closed
}

/// @brief 走查上下文：code 字节 + 可达标记。
///
/// visited 按字节标（死区段计算用）；starts 按指令头标（worklist 去重 +
/// 站点枚举用）。分支目标落在已访字节但不是任何指令头 = 语义非法，fail-
/// closed——只按 visited 去重会把 goto 的操作数字节误当指令头吞掉目标。
struct Walk
{
    using pc_t = ca::u32;

    const std::vector<ca::u8>& code;
    std::vector<bool>          visited;
    std::vector<bool>          starts;

    bool mark(pc_t pc, ca::u32 len) noexcept
    {
        if (static_cast<ca::usize>(pc) + len > code.size()) return false;
        for (ca::u32 i = 0; i < len; ++i) visited[pc + i] = true;
        starts[pc] = true;
        return true;
    }

    /// @brief 读 s2/s4 有符号跳转偏移。
    ca::i32 branchOff(pc_t pc, ca::u32 bytes) const noexcept
    {
        if (bytes == 2) {
            return static_cast<ca::i32>(
                static_cast<ca::i16>((code[pc] << 8) | code[pc + 1]));
        }
        ca::u32 u = (static_cast<ca::u32>(code[pc]) << 24)
                    | (static_cast<ca::u32>(code[pc + 1]) << 16)
                    | (static_cast<ca::u32>(code[pc + 2]) << 8)
                    | static_cast<ca::u32>(code[pc + 3]);
        return static_cast<ca::i32>(u);
    }
};

/// @brief 从一个可达 pc 展开走查。@return false = fail-closed（可达路径解不开）。
bool walkFrom(Walk& w, Walk::pc_t start)
{
    std::vector<Walk::pc_t> work{start};
    while (!work.empty()) {
        const Walk::pc_t pc = work.back();
        work.pop_back();
        if (pc >= w.code.size()) return false;
        if (w.starts[pc]) continue;              // 已解码的指令头
        if (w.visited[pc]) return false;         // 落在别的指令体内：非法字节码

        const ca::u8 op = w.code[pc];
        if (op == 0xa8 || op == 0xa9 || op == 0xc9) return false;   // jsr/ret/jsr_w：动态返回地址
        if (op == 0xc4) {                                           // wide
            if (pc + 4 > w.code.size()) return false;
            const ca::u8  sub = w.code[pc + 1];
            const ca::u32 len = (sub == 0x84) ? 6 : 4;
            if (!w.mark(pc, len)) return false;
            if (sub == 0xa9) return false;                          // wide ret
            if (pc + len < w.code.size()) work.push_back(pc + len);
            continue;
        }
        if (op == 0xaa || op == 0xab) {                             // table/lookupswitch
            // 4 字节对齐后（JVMS 6.5）：default 在 q；tableswitch 的 low/high
            // 在 q+4/q+8、case 偏移表从 q+12 起（各 4 字节）；lookupswitch 的
            // npairs 在 q+4、(match, offset) 对从 q+8 起（各 8 字节）。
            ca::usize q = pc + 1;
            const ca::usize pad = (4 - (q % 4)) % 4;
            q += pad;
            if (q + 8 > w.code.size()) return false;
            const Walk::pc_t def = pc + static_cast<ca::u32>(w.branchOff(static_cast<Walk::pc_t>(q), 4));
            ca::usize after;
            if (op == 0xaa) {
                if (q + 12 > w.code.size()) return false;
                const Walk::pc_t lo = (static_cast<Walk::pc_t>(w.code[q + 4]) << 24)
                                      | (static_cast<Walk::pc_t>(w.code[q + 5]) << 16)
                                      | (static_cast<Walk::pc_t>(w.code[q + 6]) << 8)
                                      | w.code[q + 7];
                const Walk::pc_t hi = (static_cast<Walk::pc_t>(w.code[q + 8]) << 24)
                                      | (static_cast<Walk::pc_t>(w.code[q + 9]) << 16)
                                      | (static_cast<Walk::pc_t>(w.code[q + 10]) << 8)
                                      | w.code[q + 11];
                if (lo > hi) return false;
                const ca::u64 cnt = static_cast<ca::u64>(hi) - lo + 1;
                after = q + 12 + cnt * 4;
            } else {
                const ca::i32 npairs = w.branchOff(static_cast<Walk::pc_t>(q + 4), 4);
                if (npairs < 0) return false;
                after = q + 8 + static_cast<ca::usize>(npairs) * 8;
            }
            if (after > w.code.size()) return false;
            if (def >= w.code.size()) return false;
            work.push_back(def);
            for (ca::usize t = (op == 0xaa ? q + 12 : q + 8); t + 4 <= after;) {
                // lookupswitch 的偏移在对的后半（t+4），tableswitch 在 t。
                const ca::usize offPos = (op == 0xaa) ? t : t + 4;
                const Walk::pc_t target
                    = pc + static_cast<ca::u32>(w.branchOff(static_cast<Walk::pc_t>(offPos), 4));
                if (target >= w.code.size()) return false;
                work.push_back(target);
                t += (op == 0xaa) ? 4 : 8;
            }
            if (!w.mark(pc, static_cast<ca::u32>(after - pc))) return false;
            if (after < w.code.size()) work.push_back(static_cast<Walk::pc_t>(after));
            continue;
        }

        const ca::u8 len = insnLength(op);
        if (len == 0) return false;                                 // 表外操作码
        if (!w.mark(pc, len)) return false;                         // 越界指令

        // 后继展开：分支双目标、无条件跳转/返回无直落，其余直落。
        const bool isGoto   = (op == 0xa7 || op == 0xc8);
        const bool isReturn = (op >= 0xac && op <= 0xb1) || op == 0xbf;
        const bool isBranch = (op >= 0x99 && op <= 0xa6) || op == 0xc6 || op == 0xc7;
        if (isBranch) {
            const Walk::pc_t target = pc + static_cast<ca::u32>(w.branchOff(pc + 1, 2));
            if (target >= w.code.size()) return false;
            work.push_back(target);
            work.push_back(pc + len);
        } else if (isGoto) {
            const ca::u32 offBytes = (op == 0xa7) ? 2 : 4;
            const Walk::pc_t target = pc + static_cast<ca::u32>(w.branchOff(pc + 1, offBytes));
            if (target >= w.code.size()) return false;
            work.push_back(target);
        } else if (!isReturn) {
            if (pc + len < w.code.size()) work.push_back(pc + len);
        }
    }
    return true;
}

/// @brief 一份 Code 字节的完整走查 + 死区段。
///
/// 与 junk_region_strip 的 junkRanges 同逻辑，但保留 Walk（visited/starts）
/// 给站点枚举用：可达站点按 starts 逐头找，死区站点按段线性解码找。
/// @return false = fail-closed（可达路径解不开，方法整条不动）。
bool walkMethod(const std::vector<ca::u8>& code, const jbc::core::CodeAttr& body,
                Walk& w, std::vector<std::pair<ca::u32, ca::u32>>& out)
{
    if (code.empty()) return true;
    if (!walkFrom(w, 0)) return false;
    for (const jbc::core::CodeAttr::ExcEntry& e : body.excTable) {
        if (e.handlerPc >= code.size()) return false;
        if (!walkFrom(w, e.handlerPc)) return false;
    }
    ca::u32 run = 0;
    for (ca::u32 i = 0; i < code.size(); ++i) {
        if (w.visited[i]) {
            if (run != 0) {
                out.emplace_back(i - run, run);
                run = 0;
            }
            continue;
        }
        ++run;
    }
    if (run != 0) out.emplace_back(static_cast<ca::u32>(code.size()) - run, run);
    return true;
}

/// @brief StackMapTable 帧锚面：帧声明 pc 的字节标记。
///
/// 死区 NOP 化的帧一致性问题（junk_region_strip 同款保守面）：JVM 校验器把
/// SMT 帧声明当类型状态锚点，锚状态沿 NOP 直落传播——死区尾的 `athrow` 是
/// 状态终止符（消费栈顶异常并终止直落），NOP 掉它会让锚状态泄进可达区的
/// merge 点。判据：段内无帧锚 → 全段 NOP 安全；段内有帧锚 → 段尾必须保留
/// `athrow`（NOP 化收缩 1 字节）；段内有帧锚且段尾不是 `athrow` → 整方法
/// fail-closed（锚会泄进可达区，不猜）。SMT 属性解码失败 = 无法知道锚在哪
/// → 整方法 fail-closed（宁留不猜）。
bool frameAnchors(jbc::core::ClassFile& cf, jbc::core::CodeAttr& body,
                  std::vector<bool>& hasFrame)
{
    using jbc::core::StackMapTableAttr;
    for (jbc::core::AttrSlot& slot : body.attrs) {
        if (cf.utf8(slot.nameCp) != "StackMapTable") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        const auto* smt = static_cast<const StackMapTableAttr*>(d.unwrap());
        ca::u32         prev = static_cast<ca::u32>(-1);   /// 首帧 delta 即 pc（JVMS 4.7.4）。
        for (const StackMapTableAttr::Frame& f : smt->frames) {
            ca::u32 delta = 0;
            if (f.type <= StackMapTableAttr::Frame::kSameMax) {
                delta = f.type;
            } else if (f.type >= StackMapTableAttr::Frame::kSame1Base
                       && f.type <= StackMapTableAttr::Frame::kSame1Max) {
                delta = f.type - StackMapTableAttr::Frame::kSame1Base;
            } else if (f.type == StackMapTableAttr::Frame::kSame1Ext
                       || (f.type >= StackMapTableAttr::Frame::kChopMax
                           && f.type <= StackMapTableAttr::Frame::kFull)) {
                delta = f.offset;
            } else {
                return false;   /// 128..246 保留标签：不猜。
            }
            const ca::u32 pc = prev + delta + 1;   /// smtAbsPc 语义（wrap 起点自洽）。
            if (static_cast<ca::usize>(pc) < hasFrame.size()) hasFrame[pc] = true;
            prev = pc;
        }
    }
    return true;
}

// ============================================================================
// ==== 诱饵普查（CP 面）=======================================================
// ============================================================================

/// @brief 单类诱饵普查：标记命中 Utf8、String 包装数、引用面与孤儿。
///
/// 引用面口径（宁误留）：全部 CP 条目的 operand 槽（String/Class/
/// MethodType/Module/Package → a；NameAndType/引用族 → a+b；MethodHandle →
/// b（a 是 reference_kind）；Dynamic/InvokeDynamic → b（a 是 bootstrap 下
/// 标））+ 成员 name/desc + 属性名槽（类级与成员级；Code 嵌套属性名是固定
/// 词汇表，不可能承载诱饵文本，不遍历）+ 代码面 ldc/ldc_w 操作数的线性
/// 扫描（CONSTANT_String 的唯一字节码引用面；操作数字节的误命中只多算
/// 引用，fail-closed 方向）。属性体内的直连 Utf8 引用（Signature/注解
/// typeCp 一族）不在普查内——实际不可能承载 228 字节诱饵文本，且压缩闭
/// 包兜底（见头注）。
///
/// 闭包死（removable）：Utf8 直连零引用，或引用面全部是 CONSTANT_String
/// 且这些 String 零 CP 引用、零代码 LDC 引用（死 LDC NOP 化后 String 与
/// Utf8 同轮孤儿化的形态；单看 Utf8 直引用永远非零，摘除必须连包装一起
/// 收）。Code 属性解不开 → 代码面引用不可知，removable 只收直连孤儿。
struct ClassBait
{
    std::vector<ca::u32>  utf8s;      ///< 标记命中的 Utf8 索引
    std::set<std::string> distinct;   ///< distinct 文本
    ca::usize             strings   = 0;   ///< 引用诱饵 Utf8 的 CONSTANT_String 数
    ca::usize             orphans   = 0;   ///< 直连零引用孤儿
    ca::usize             removable = 0;   ///< 闭包死条目
};

ClassBait censusClass(jbc::core::ClassFile& cf)
{
    ClassBait                      out;
    const jbc::core::ConstantPool& cp = cf.cp;
    std::vector<bool>              isBait(cp.size(), false);
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (!cp.isUtf8(i)) continue;
        if (isBaitContent(cp.utf8Of(i))) {
            isBait[i] = true;
            out.utf8s.push_back(i);
            out.distinct.emplace(std::string(cp.utf8Of(i)));
        }
    }
    if (out.utf8s.empty()) return out;

    std::vector<std::vector<ca::u32>> refsFrom(cp.size());
    const auto bump = [&](ca::u32 from, ca::u32 idx) {
        if (idx != 0 && idx < refsFrom.size()) refsFrom[idx].push_back(from);
    };
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (!cp.valid(i)) continue;
        const jbc::core::ConstantPool::Entry& e = cp.at(i);
        using T                                 = jbc::core::CpTag;
        switch (e.tag) {
        case T::Utf8:
        case T::Integer:
        case T::Float:
        case T::Long:
        case T::Double:
            break;                                       // 叶子：无出边
        case T::MethodHandle:
        case T::Dynamic:
        case T::InvokeDynamic:
            bump(i, e.b);                                // a 列非 CP 索引
            break;
        default:
            bump(i, e.a);
            if (e.tag == T::Fieldref || e.tag == T::Methodref
                || e.tag == T::InterfaceMethodref || e.tag == T::NameAndType) {
                bump(i, e.b);
            }
            break;
        }
        if (e.tag == T::String && e.a != 0 && e.a < cp.size() && isBait[e.a]) {
            ++out.strings;
        }
    }
    // 成员与属性名槽（直连 Utf8 索引的面；from 索引用成员/槽自身下标的
    // 代理值——只进计数列，不参与闭包 tag 判断，占位即可）。
    constexpr ca::u32 kNameSlotFrom = 0xFFFF0000;
    const auto bumpMember = [&](const jbc::core::MemberInfo& m) {
        bump(kNameSlotFrom, m.nameCp);
        bump(kNameSlotFrom, m.descCp);
        for (const jbc::core::AttrSlot& slot : m.attrs) bump(kNameSlotFrom, slot.nameCp);
    };
    for (const jbc::core::AttrSlot& slot : cf.attrs) bump(kNameSlotFrom, slot.nameCp);
    for (const jbc::core::MemberInfo& f : cf.fields) bumpMember(f);
    for (const jbc::core::MemberInfo& m : cf.methods) bumpMember(m);

    // 代码面 ldc/ldc_w 操作数线性扫描（CONSTANT_String 的字节码引用面；
    // 线性误命中只多算引用，宁误留方向）。
    std::vector<ca::usize> codeRefs(cp.size(), 0);
    const auto bumpCode = [&](ca::u32 idx) {
        if (idx != 0 && idx < codeRefs.size()) ++codeRefs[idx];
    };
    bool codeBlind = false;
    for (jbc::core::MemberInfo& m : cf.methods) {
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) {
                codeBlind = true;
                continue;
            }
            const auto&                body = static_cast<const jbc::core::CodeAttr&>(*d.unwrap());
            const std::vector<ca::u8>& code = body.code;
            for (ca::usize p = 0; p + 1 < code.size(); ++p) {
                if (code[p] == kOpLdc) {
                    bumpCode(code[p + 1]);
                    continue;
                }
                if (code[p] == kOpLdcW && p + 2 < code.size()) {
                    bumpCode((static_cast<ca::u32>(code[p + 1]) << 8)
                             | static_cast<ca::u32>(code[p + 2]));
                }
            }
        }
    }

    for (const ca::u32 u : out.utf8s) {
        if (refsFrom[u].empty()) {
            ++out.orphans;
            ++out.removable;
            continue;
        }
        // 闭包死：引用面全部是零引用（CP + 代码面）的 CONSTANT_String。
        bool wrappedOnly = true;
        for (const ca::u32 r : refsFrom[u]) {
            if (cp.at(r).tag != jbc::core::CpTag::String) {
                wrappedOnly = false;
                break;
            }
            if (refsFrom[r].empty() && codeRefs[r] == 0) continue;
            wrappedOnly = false;
            break;
        }
        if (wrappedOnly && !codeBlind) ++out.removable;
    }
    static const bool baitTrace = std::getenv("GRUNT_BAIT_TRACE") != nullptr;
    if (baitTrace) {
        std::fprintf(stderr,
                     "[grunt.BaitStrip] census: utf8s=%zu strings=%zu orphans=%zu"
                     " removable=%zu codeBlind=%d\n",
                     out.utf8s.size(), out.strings, out.orphans, out.removable,
                     codeBlind ? 1 : 0);
    }
    return out;
}

// ============================================================================
// ==== 站点分类（Code 面）=====================================================
// ============================================================================

/// @brief LDC 操作数解析到本类诱饵 Utf8 才算站点；返回 Utf8 索引或 kNpos。
ca::usize baitUtf8OfOperand(const jbc::core::ClassFile& cf, const std::vector<bool>& isBait,
                            ca::u32 cpIdx)
{
    const jbc::core::ConstantPool& cp = cf.cp;
    if (cpIdx == 0 || cpIdx >= cp.size() || !cp.valid(cpIdx)) return kNpos;
    const jbc::core::ConstantPool::Entry& e = cp.at(cpIdx);
    if (e.tag != jbc::core::CpTag::String) return kNpos;
    if (e.a == 0 || e.a >= cp.size() || !cp.valid(e.a) || !isBait[e.a]) return kNpos;
    return e.a;
}

/// @brief 一个 Code 体的站点分类结果。
struct MethodSites
{
    std::vector<BaitSite>                    sites;      ///< 全部已分类站点
    std::vector<std::pair<ca::u32, ca::u32>> nopRanges;  ///< 锚修正后的死区段
    ca::usize stripped  = 0;
    ca::usize blocked   = 0;
    ca::usize deadUndecoded = 0;
    bool      decodeFail = false;   ///< Code 属性解码失败（站点不可知）
    bool      failClosed = false;   ///< 走查失败 / SMT 解不开 / 锚尾非 athrow
};

/// @brief 一个 Code 体上的诱饵站点普查与分类（SCAN 的方法面本体）。
///
/// 可达站点按 starts 逐指令头找；死区站点按段线性解码找（死区起点必是
/// 指令边界——可达指令的字节跨度全部被走查标记过）。锚修正按
/// junk_region_strip 口径：段内无锚全段 NOP；有锚段尾 athrow 保留（收缩
/// 1 字节）；有锚段尾非 athrow 或 SMT 解不开 → 整方法 fail-closed。
MethodSites classifyMethod(const std::string& uri, const std::vector<bool>& isBait,
                           jbc::core::ClassFile& cf, ca::usize methodIndex,
                           ca::usize codeSlot, jbc::core::CodeAttr& body)
{
    MethodSites out;
    const std::vector<ca::u8>& code = body.code;

    Walk w{code, std::vector<bool>(code.size(), false),
           std::vector<bool>(code.size(), false)};
    std::vector<std::pair<ca::u32, ca::u32>> ranges;
    if (!walkMethod(code, body, w, ranges)) {
        out.failClosed = true;
        return out;
    }

    // 可达站点：逐指令头（starts 上的字节必是操作码）。
    for (ca::u32 pc = 0; pc < code.size(); ++pc) {
        if (!w.starts[pc]) continue;
        const ca::u8 op = code[pc];
        if (op != kOpLdc && op != kOpLdcW) continue;
        if (static_cast<ca::usize>(pc) + 2 > code.size()) continue;
        const ca::u32 cpIdx = (op == kOpLdc)
                                  ? static_cast<ca::u32>(code[pc + 1])
                                  : (static_cast<ca::u32>(code[pc + 1]) << 8)
                                        | static_cast<ca::u32>(code[pc + 2]);
        const ca::usize u = baitUtf8OfOperand(cf, isBait, cpIdx);
        if (u == kNpos) continue;
        BaitSite site;
        site.uri       = uri;
        site.methodIndex = methodIndex;
        site.codeSlot  = codeSlot;
        site.pc        = pc;
        site.reachable = true;
        site.utf8Cp    = static_cast<ca::u32>(u);
        out.sites.push_back(std::move(site));
    }

    // 死区站点：段内线性解码（段起点必是指令边界）。
    struct BaitRange
    {
        ca::u32                  start = 0;
        ca::u32                  len   = 0;
        std::vector<std::pair<ca::u32, ca::u32>> pcs;   ///< 段内诱饵站点 (pc, utf8Cp)
    };
    std::vector<BaitRange> baitRanges;
    for (const auto& [start, len] : ranges) {
        BaitRange             br{start, len, {}};
        ca::u32               pc  = start;
        const ca::u32         end = start + len;
        while (pc < end) {
            const ca::u8 op = code[pc];
            if (op == kOpLdc || op == kOpLdcW) {
                ca::u32 operand = 2;
                ca::u32 cpIdx   = 0;
                if (op == kOpLdc) {
                    if (pc + 2 > end) {
                        out.deadUndecoded += end - pc;
                        break;
                    }
                    cpIdx = static_cast<ca::u32>(code[pc + 1]);
                } else {
                    operand = 3;
                    if (pc + 3 > end) {
                        out.deadUndecoded += end - pc;
                        break;
                    }
                    cpIdx = (static_cast<ca::u32>(code[pc + 1]) << 8)
                            | static_cast<ca::u32>(code[pc + 2]);
                }
                const ca::usize u = baitUtf8OfOperand(cf, isBait, cpIdx);
                if (u != kNpos) br.pcs.emplace_back(pc, static_cast<ca::u32>(u));
                pc += operand;
                continue;
            }
            const ca::u8 ln = insnLength(op);
            if (ln == 0 || static_cast<ca::usize>(pc) + ln > end) {
                out.deadUndecoded += end - pc;   // 表外/变长/截断：线性解码盲区
                break;
            }
            pc += ln;
        }
        if (!br.pcs.empty()) baitRanges.push_back(std::move(br));
    }

    // 死区站点先落账（fail-closed 路径上它们是有死证但手段外的 blocked）。
    for (const BaitRange& br : baitRanges) {
        for (const auto& [pc, u] : br.pcs) {
            BaitSite site;
            site.uri         = uri;
            site.methodIndex = methodIndex;
            site.codeSlot    = codeSlot;
            site.pc          = pc;
            site.reachable   = false;
            site.utf8Cp      = u;
            site.range       = {br.start, br.len};
            out.sites.push_back(std::move(site));
        }
    }

    // fail-closed 收口：站点已知的路径把全部死区站点记 blocked（有死证、
    // 手段外）；站点不可知的路径（走查失败）由 scan 的 methodsFailClosed 记。
    const auto closeBlocked = [&out]() {
        out.failClosed = true;
        for (const BaitSite& s : out.sites) {
            if (!s.reachable) ++out.blocked;
        }
        return out;
    };

    if (baitRanges.empty()) return out;

    // 帧锚保守面：SMT 解不开 → 整方法 fail-closed（锚在哪不可知，不猜）。
    std::vector<bool> hasFrame(code.size(), false);
    if (!frameAnchors(cf, body, hasFrame)) {
        return closeBlocked();
    }

    // 锚修正：任一含诱饵站点死区段不满足保留条件 → 整方法 fail-closed
    // （junk_region_strip 同款：锚会泄进可达区，不猜）。
    std::vector<std::pair<ca::u32, ca::u32>> corrected;
    for (const BaitRange& br : baitRanges) {
        bool anchored = false;
        for (ca::u32 i = 0; i < br.len; ++i) {
            if (hasFrame[br.start + i]) {
                anchored = true;
                break;
            }
        }
        if (!anchored) {
            corrected.emplace_back(br.start, br.len);
            continue;
        }
        const ca::u32 tailPc = br.start + br.len - 1;
        if (code[tailPc] != 0xbf) {              // athrow 是唯一可保留终止符
            return closeBlocked();
        }
        corrected.emplace_back(br.start, br.len - 1);   // 段尾 athrow 保留
    }

    // 站点归类落账：修正段内 = stripped。
    out.nopRanges = std::move(corrected);
    ca::usize stripped = 0;
    for (BaitSite& s : out.sites) {
        if (s.reachable) continue;
        bool inJob = false;
        for (const auto& [start, len] : out.nopRanges) {
            if (s.pc >= start && s.pc < start + len) {
                inJob = true;
                break;
            }
        }
        if (inJob) ++stripped;
    }
    out.stripped = stripped;
    return out;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult              result;
    std::set<std::string>   allDistinct;

    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        jbc::core::ClassFile& cf = session->backing();
        const std::string     uri(entry.uri);

        const ClassBait bait = censusClass(cf);
        if (bait.utf8s.empty()) return;
        std::vector<bool> isBait(cf.cp.size(), false);
        for (const ca::u32 u : bait.utf8s) isBait[u] = true;

        result.baitUtf8Count += bait.utf8s.size();
        result.baitStrings += bait.strings;
        result.orphanUtf8 += bait.orphans;
        result.removableUtf8 += bait.removable;
        allDistinct.insert(bait.distinct.begin(), bait.distinct.end());
        result.baitUris.push_back(uri);
        ++result.classesWithBait;

        for (ca::usize mi = 0; mi < cf.methods.size(); ++mi) {
            jbc::core::MemberInfo& m = cf.methods[mi];
            ca::usize codeSlot = kNpos;
            for (ca::usize i = 0; i < m.attrs.size(); ++i) {
                if (cf.utf8(m.attrs[i].nameCp) == "Code") {
                    codeSlot = i;
                    break;
                }
            }
            if (codeSlot == kNpos) continue;
            auto d = cf.decodeAttr(m.attrs[codeSlot]);
            if (d.is_err()) {
                ++result.methodsDecodeFail;
                continue;
            }
            auto& body = static_cast<jbc::core::CodeAttr&>(*d.unwrap());

            MethodSites ms = classifyMethod(uri, isBait, cf, mi, codeSlot, body);
            if (ms.decodeFail) {
                ++result.methodsDecodeFail;
                continue;
            }
            static const bool baitTrace = std::getenv("GRUNT_BAIT_TRACE") != nullptr;
            if (baitTrace && (!ms.sites.empty() || ms.failClosed || ms.deadUndecoded != 0)) {
                std::fprintf(stderr,
                             "[grunt.BaitStrip] method %s#%zu: sites=%zu reach=%zu"
                             " stripped=%zu blocked=%zu failClosed=%d deadUndecoded=%zu\n",
                             uri.c_str(), mi, ms.sites.size(), ms.sites.size() - [&] {
                                 ca::usize n = 0;
                                 for (const auto& s : ms.sites) n += s.reachable ? 0 : 1;
                                 return n;
                             }(),
                             ms.stripped, ms.blocked, ms.failClosed ? 1 : 0,
                             ms.deadUndecoded);
            }
            result.methodsFailClosed += ms.failClosed ? 1 : 0;
            result.deadUndecodedBytes += ms.deadUndecoded;
            for (BaitSite& s : ms.sites) {
                result.reachableSites += s.reachable ? 1 : 0;
                result.unreachableSites += s.reachable ? 0 : 1;
                result.sites.push_back(std::move(s));
            }
            result.strippedSites += ms.stripped;
            result.blockedSites += ms.blocked;
            if (!ms.nopRanges.empty()) {
                NopJob job;
                job.uri         = uri;
                job.methodIndex = mi;
                job.codeSlot    = codeSlot;
                job.ranges      = std::move(ms.nopRanges);
                job.siteCount   = ms.stripped;
                result.nops.push_back(std::move(job));
            }
        }
    });
    // liveSites = reachableSites（无死证站点一律保留；守卫求值面不在本 pass）。
    result.liveSites = result.reachableSites;
    result.distinctStrings = allDistinct.size();
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // (1) NOP 作业：按 scan 定死的段写 0x00（长度保持），不回看判据。
    {
        std::unordered_map<std::string, std::vector<const NopJob*>> byUri;
        for (const NopJob& job : result.nops) byUri[job.uri].push_back(&job);
        for (auto& [uri, jobs] : byUri) {
            const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
            if (bytes == nullptr) continue;
            auto session = openSession(*bytes);
            if (!session) continue;
            jbc::core::ClassFile& cf = session->backing();

            bool    changed  = false;
            ca::usize stripped = 0;
            ca::usize nopBytes = 0;
            for (const NopJob* job : jobs) {
                if (job->methodIndex >= cf.methods.size()
                    || job->codeSlot >= cf.methods[job->methodIndex].attrs.size()) {
                    ++dirty.sitesOutOfRange;
                    continue;
                }
                jbc::core::AttrSlot& slot
                    = cf.methods[job->methodIndex].attrs[job->codeSlot];
                if (cf.decodeAttr(slot).is_err()) {
                    ++dirty.sitesOutOfRange;
                    continue;
                }
                auto* bodyPtr = slot.mutate();   // 置 Modified：写回重编码本槽
                if (bodyPtr == nullptr) continue;
                auto& body = static_cast<jbc::core::CodeAttr&>(*bodyPtr);
                ca::usize jobBytes = 0;
                for (const auto& [start, len] : job->ranges) {
                    if (static_cast<ca::usize>(start) + len > body.code.size()) continue;
                    for (ca::u32 i = 0; i < len; ++i) body.code[start + i] = kNop;
                    jobBytes += len;
                }
                if (jobBytes == 0) continue;
                nopBytes += jobBytes;
                stripped += job->siteCount;
                changed = true;
            }
            if (!changed) continue;
            auto encoded = jbc::core::writeClass(cf);
            if (encoded.is_err()) continue;
            replaceClass(container, uri, std::move(encoded).unwrap());
            dirty.uris.push_back(uri);
            dirty.strippedSites += stripped;
            dirty.nopBytes += nopBytes;
        }
    }

    // (2) 孤儿诱饵 CP 摘除：每类重开当前字节重做普查（NOP 化新孤儿化 +
    //     scan 时点孤儿一并收口），有孤儿才压缩；摘除量按压缩后重扫实测。
    {
        std::set<std::string> nopWritten(dirty.uris.begin(), dirty.uris.end());
        for (const std::string& uri : result.baitUris) {
            const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
            if (bytes == nullptr) continue;
            auto session = openSession(*bytes);
            if (!session) continue;
            const ClassBait before = censusClass(session->backing());
            if (before.removable == 0) continue;   // 全部诱饵仍有活引用：不动
            // 注解族深度预检（mjbase #158）：cp_compact 解全部属性槽。
            if (!classAnnoDepthSafe(session->backing())) {
                ++dirty.compactFailed;
                continue;
            }
            if (ir::compactConstantPool(*session).is_err()) {
                ++dirty.compactFailed;
                continue;
            }
            auto encoded = jbc::core::writeClass(session->backing());
            if (encoded.is_err()) {
                ++dirty.compactFailed;
                continue;
            }
            std::vector<ca::u8> fresh = std::move(encoded).unwrap();
            if (fresh == *bytes) continue;       // 无实际变化（普查误报面）：不落盘
            // 摘除量实测：压缩后的新字节重扫（不按普查假设记账）。
            auto afterSession = openSession(fresh);
            ca::usize removedUtf8    = 0;
            ca::usize removedStrings = 0;
            if (afterSession) {
                const ClassBait after = censusClass(afterSession->backing());
                removedUtf8    = before.utf8s.size() >= after.utf8s.size()
                                     ? before.utf8s.size() - after.utf8s.size()
                                     : 0;
                removedStrings = before.strings >= after.strings
                                     ? before.strings - after.strings
                                     : 0;
            }
            if (removedUtf8 == 0 && removedStrings == 0) {
                // 压缩动了非诱饵死条目（普查口径差）：本 pass 不落这盘字节，
                // 保原样（改动面只在确有诱饵摘除的类）。
                ++dirty.compactNoBait;
                continue;
            }
            replaceClass(container, uri, std::move(fresh));
            dirty.removedUtf8 += removedUtf8;
            dirty.removedStrings += removedStrings;
            ++dirty.compactedClasses;
            if (nopWritten.count(uri) == 0) dirty.uris.push_back(uri);
        }
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    ProofResult result;
    // 类数检查：本 pass 不摘类，类数必须不增不减。
    if (countClasses(container) != classesBefore) {
        result.ok = false;
        result.failed.emplace_back("class-count");
    }
    // 落盘类可重新解析（会话往返）。
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
    // residual 重扫（挂起时间片：全容器不变量不许被超墙截短）：按内容判据
    // 数剩余诱饵 Utf8，站点数按同一分类器重走。
    {
        pass_clock::Suspend suspend;
        ca::usize           sites = 0;
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            jbc::core::ClassFile& cf = session->backing();
            const ClassBait bait = censusClass(cf);
            result.residualBaitUtf8 += bait.utf8s.size();
            if (bait.utf8s.empty()) return;
            std::vector<bool> isBait(cf.cp.size(), false);
            for (const ca::u32 u : bait.utf8s) isBait[u] = true;
            for (ca::usize mi = 0; mi < cf.methods.size(); ++mi) {
                jbc::core::MemberInfo& m = cf.methods[mi];
                ca::usize codeSlot = kNpos;
                for (ca::usize i = 0; i < m.attrs.size(); ++i) {
                    if (cf.utf8(m.attrs[i].nameCp) == "Code") {
                        codeSlot = i;
                        break;
                    }
                }
                if (codeSlot == kNpos) continue;
                auto d = cf.decodeAttr(m.attrs[codeSlot]);
                if (d.is_err()) continue;
                auto&        body = static_cast<jbc::core::CodeAttr&>(*d.unwrap());
                MethodSites  ms
                    = classifyMethod(std::string(entry.uri), isBait, cf, mi, codeSlot, body);
                sites += ms.sites.size();
            }
        });
        result.residualSites = sites;
    }
    return result;
}

}   // namespace grunt::bait_strip
