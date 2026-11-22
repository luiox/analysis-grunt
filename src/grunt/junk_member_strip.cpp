/// @file        junk_member_strip.cpp
/// @brief       残留 `junk_member_strip` 的三段实现：SCAN / REWRITE / PROOF。
///
/// 判据等级与保守面（判据全文见头注）：junk 成员按**名字形**预收（数字开头
/// / `$g` 前缀，普查背书两 jar 名形全集恰为这两族），删除面由改写后的全容
/// 器活引用普查关死；NOP 区域按「junk CFG 闭包 + 改写后模拟 + 孤儿容许 +
/// 单夹层真实块」判定，任何一环看不清都整方法放弃（fail-closed，字节不动）。
///
/// scan/rewrite 分工：scan 产出定死的 NOP 段与成员候选（纯读，不碰容器）；
/// rewrite 按段写 0x00（长度保持）→ 压缩 → 普查 → 删成员 → 再压缩。区域
/// 判定全部发生在 scan，rewrite 不回看判据。
///
/// 槽位效应表：完整 JVMS 6.5 表（字段/调用按描述符解析，J/D 双槽），与
/// junk_region_strip 的操作码精确长度表同源不同用（这里还要栈深）。
#include "grunt/junk_member_strip.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "grunt/container.hpp"
#include "grunt/pass_clock.hpp"

namespace grunt::junk_member_strip {

namespace {

namespace ir = jbc::ir;

constexpr ca::u8 kNop = 0x00;

// ---------------------------------------------------------------------------
// 名字形（普查背书：dec_jdk / v3_full_obf 两容器 junk 名全集恰为两族）
// ---------------------------------------------------------------------------

/// @brief junk 形字段名：数字开头（字符串池诱饵族）或 `$g` 前缀（junk 键族）。
bool junkFieldName(std::string_view name) noexcept
{
    if (name.empty()) return false;
    if (name[0] >= '0' && name[0] <= '9') return true;
    return name.size() >= 3 && name[0] == '$' && name[1] == 'g';
}

/// @brief junk 形方法名：`$g` 前缀（探针族，普查 260/260 全为此形）。
bool junkMethodName(std::string_view name) noexcept
{
    return name.size() >= 3 && name[0] == '$' && name[1] == 'g';
}

// ---------------------------------------------------------------------------
// 操作码精确长度表（JVMS 6.5；与 junk_region_strip 同表；变长族特判）
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// 线性解码
// ---------------------------------------------------------------------------

/// @brief 一条解码出的指令。
struct Insn
{
    ca::u32 pc  = 0;
    ca::u32 op  = 0;
    ca::u32 len = 0;      ///< 含操作码的字节数
    ca::u32 cp  = 0;      ///< 引用族操作的 CP 索引（0 = 无）
    ca::i32 arg = 0;      ///< 分支偏移（相对 pc）/ switch default 相对偏移
    std::vector<ca::u32> swTargets;   ///< switch 全部 case 目标（绝对 pc）
};

/// @brief 一个方法的线性解码体（Code 原始字节面，不 promote）。
struct Body
{
    std::vector<Insn>                 insns;
    std::unordered_map<ca::u32, ca::usize> byPc;
    std::vector<ca::u32>              handlers;   ///< 异常 handler pc
    bool                              ok = false; ///< 全量解码成功（无表外/wide 截断）
};

/// @brief 读 s2/s4 有符号量。
ca::i32 readS2(const std::vector<ca::u8>& code, ca::u32 pc) noexcept
{
    return static_cast<ca::i32>(
        static_cast<ca::i16>((code[pc] << 8) | code[pc + 1]));
}

ca::i32 readS4(const std::vector<ca::u8>& code, ca::u32 pc) noexcept
{
    const ca::u32 u = (static_cast<ca::u32>(code[pc]) << 24)
                      | (static_cast<ca::u32>(code[pc + 1]) << 16)
                      | (static_cast<ca::u32>(code[pc + 2]) << 8)
                      | static_cast<ca::u32>(code[pc + 3]);
    return static_cast<ca::i32>(u);
}

/// @brief Code 字节线性解码。表外操作码 / wide / switch 解不开时 ok=false
///        （sim 面按 fail-closed 处理，与原型口径一致）。
Body decodeBody(const std::vector<ca::u8>& code)
{
    Body body;
    body.ok = true;
    const ca::u32 n = static_cast<ca::u32>(code.size());
    ca::u32       pc = 0;
    while (pc < n) {
        const ca::u8 op = code[pc];
        Insn         in;
        in.pc = pc;
        in.op = op;
        if (op == 0xc4) {                       // wide：本 pass 不在白名单
            body.ok = false;
            return body;
        }
        if (op == 0xaa || op == 0xab) {         // switch：变长
            ca::usize q = pc + 1;
            q += (4 - (q % 4)) % 4;
            if (q + 8 > code.size()) {
                body.ok = false;
                return body;
            }
            in.arg = readS4(code, static_cast<ca::u32>(q));
            if (op == 0xaa) {
                if (q + 12 > code.size()) {
                    body.ok = false;
                    return body;
                }
                const ca::i32 lo = readS4(code, static_cast<ca::u32>(q + 4));
                const ca::i32 hi = readS4(code, static_cast<ca::u32>(q + 8));
                if (lo > hi) {
                    body.ok = false;
                    return body;
                }
                const ca::u64 cnt = static_cast<ca::u64>(hi) - lo + 1;
                const ca::usize after = q + 12 + static_cast<ca::usize>(cnt) * 4;
                if (after > code.size()) {
                    body.ok = false;
                    return body;
                }
                in.len = static_cast<ca::u32>(after - pc);
                for (ca::u64 k = 0; k < cnt; ++k) {
                    in.swTargets.push_back(
                        static_cast<ca::u32>(static_cast<ca::i64>(pc)
                                             + readS4(code,
                                                      static_cast<ca::u32>(
                                                          q + 12 + k * 4))));
                }
            } else {
                const ca::i32 np = readS4(code, static_cast<ca::u32>(q + 4));
                if (np < 0) {
                    body.ok = false;
                    return body;
                }
                const ca::usize after = q + 8 + static_cast<ca::usize>(np) * 8;
                if (after > code.size()) {
                    body.ok = false;
                    return body;
                }
                in.len = static_cast<ca::u32>(after - pc);
                for (ca::i32 k = 0; k < np; ++k) {
                    in.swTargets.push_back(
                        static_cast<ca::u32>(static_cast<ca::i64>(pc)
                                             + readS4(code,
                                                      static_cast<ca::u32>(
                                                          q + 8
                                                          + static_cast<ca::usize>(k)
                                                                * 8 + 4))));
                }
            }
        } else {
            const ca::u8 len = insnLength(op);
            if (len == 0) {                     // 表外
                body.ok = false;
                return body;
            }
            if (static_cast<ca::usize>(pc) + len > code.size()) {
                body.ok = false;
                return body;
            }
            in.len = len;
            // CP 引用族与分支偏移
            if (op == 0x12) {
                in.cp = code[pc + 1];
            } else if (op == 0x13 || op == 0x14) {
                in.cp = (static_cast<ca::u32>(code[pc + 1]) << 8) | code[pc + 2];
            } else if (op == 0x10) {
                in.arg = static_cast<ca::i8>(code[pc + 1]);
            } else if (op == 0x11) {
                in.arg = readS2(code, pc + 1);
            } else if ((op >= 0x99 && op <= 0xa6) || op == 0xa7 || op == 0xa8
                       || op == 0xc6 || op == 0xc7) {
                in.arg = readS2(code, pc + 1);
            } else if (op == 0xc8 || op == 0xc9) {
                in.arg = readS4(code, pc + 1);
            } else if (op == 0x84) {
                in.arg = static_cast<ca::i8>(code[pc + 2]);
            } else if ((op >= 0xb2 && op <= 0xb8) || op == 0xb9 || op == 0xba
                       || op == 0xbb || op == 0xbd || op == 0xc0 || op == 0xc1) {
                in.cp = (static_cast<ca::u32>(code[pc + 1]) << 8) | code[pc + 2];
            } else if (op == 0xbc) {
                in.arg = code[pc + 1];
            } else if (op == 0xc5) {
                in.cp  = (static_cast<ca::u32>(code[pc + 1]) << 8) | code[pc + 2];
                in.arg = code[pc + 3];
            }
        }
        body.byPc.emplace(pc, body.insns.size());
        body.insns.push_back(in);
        pc += in.len;
    }
    return body;
}

/// @brief 后继 pc 集（含 switch 全部 case 目标）。
std::vector<ca::u32> successors(const Body& body, ca::usize idx)
{
    const Insn& in = body.insns[idx];
    std::vector<ca::u32> out;
    const bool isExit = (in.op >= 0xac && in.op <= 0xb1) || in.op == 0xbf;
    if (isExit) return out;
    if (in.op == 0xa7 || in.op == 0xa8 || in.op == 0xc8 || in.op == 0xc9) {
        out.push_back(static_cast<ca::u32>(static_cast<ca::i64>(in.pc) + in.arg));
        return out;
    }
    if (in.op == 0xaa || in.op == 0xab) {
        out.push_back(static_cast<ca::u32>(static_cast<ca::i64>(in.pc) + in.arg));
        out.insert(out.end(), in.swTargets.begin(), in.swTargets.end());
        return out;
    }
    if ((in.op >= 0x99 && in.op <= 0xa6) || in.op == 0xc6 || in.op == 0xc7) {
        out.push_back(static_cast<ca::u32>(static_cast<ca::i64>(in.pc) + in.arg));
    }
    out.push_back(in.pc + in.len);
    return out;
}

// ---------------------------------------------------------------------------
// CP 引用解析
// ---------------------------------------------------------------------------

struct Ref
{
    ca::u16     tag  = 0;
    std::string owner;
    std::string name;
    std::string desc;
};

bool resolveRef(const jbc::core::ClassFile& cf, ca::u32 idx, Ref& out)
{
    const jbc::core::ConstantPool& cp = cf.cp;
    if (idx == 0 || idx >= cp.size() || !cp.valid(idx)) return false;
    const auto& e = cp.at(idx);
    using T = jbc::core::CpTag;
    if (e.tag != T::Fieldref && e.tag != T::Methodref && e.tag != T::InterfaceMethodref) {
        return false;
    }
    if (!cp.valid(e.a) || !cp.valid(e.b)) return false;
    const auto& cls = cp.at(e.a);
    const auto& nat = cp.at(e.b);
    if (cls.tag != T::Class || nat.tag != T::NameAndType) return false;
    if (!cp.valid(cls.a) || !cp.valid(nat.a) || !cp.valid(nat.b)) return false;
    out.tag  = static_cast<ca::u16>(e.tag);
    out.owner = std::string(cf.utf8(cls.a));
    out.name  = std::string(cf.utf8(nat.a));
    out.desc  = std::string(cf.utf8(nat.b));
    return true;
}

/// @brief ldc/ldc_w/ldc2_w 载荷是否数值常量（Integer/Float/Long/Double）。
bool cpIsNumericConst(const jbc::core::ClassFile& cf, ca::u32 idx)
{
    const jbc::core::ConstantPool& cp = cf.cp;
    if (idx == 0 || idx >= cp.size() || !cp.valid(idx)) return false;
    const auto& e = cp.at(idx);
    using T = jbc::core::CpTag;
    return e.tag == T::Integer || e.tag == T::Float || e.tag == T::Long
           || e.tag == T::Double;
}

// ---------------------------------------------------------------------------
// 描述符槽位
// ---------------------------------------------------------------------------

/// @brief 方法描述符实参槽数（J/D 各占 2 槽）。
ca::u32 descArgSlots(std::string_view desc)
{
    ca::u32 n   = 0;
    ca::usize i = 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        if (c == 'J' || c == 'D') {
            n += 2;
            i += 1;
        } else if (c == 'L') {
            n += 1;
            const ca::usize semi = desc.find(';', i);
            if (semi == std::string_view::npos) return n;
            i = semi + 1;
        } else if (c == '[') {
            n += 1;
            i += 1;
            while (i < desc.size() && desc[i] == '[') {
                i += 1;
            }
            if (i < desc.size() && desc[i] == 'L') {
                const ca::usize semi = desc.find(';', i);
                if (semi == std::string_view::npos) return n;
                i = semi + 1;
            } else {
                i += 1;
            }
        } else {
            n += 1;
            i += 1;
        }
    }
    return n;
}

/// @brief 方法描述符返回值槽数（V=0，J/D=2，其余 1）。
ca::u32 descRetSlots(std::string_view desc)
{
    const ca::usize r = desc.rfind(')');
    if (r == std::string_view::npos || r + 1 >= desc.size()) return 1;
    const char c = desc[r + 1];
    if (c == 'V') return 0;
    if (c == 'J' || c == 'D') return 2;
    return 1;
}

/// @brief 字段描述符槽位数（J/D=2，其余 1）。
ca::u32 fieldSlots(std::string_view desc) noexcept
{
    return (desc == "J" || desc == "D") ? 2 : 1;
}

// ---------------------------------------------------------------------------
// 槽位效应（JVMS 6.5 全表；字段/调用按描述符）
// ---------------------------------------------------------------------------

struct Eff
{
    ca::u32 pop  = 0;
    ca::u32 push = 0;
};

/// @brief 单指令槽位效应；@return false = 效应未知（模拟放弃）。
bool slotEffect(const jbc::core::ClassFile& cf, const Insn& in, Eff& out)
{
    const ca::u32 op = in.op;
    if (op <= 0x01) { out = {0, op == 0x01 ? 1u : 0u}; return true; }
    if (op <= 0x08) { out = {0, 1}; return true; }              // iconst_m1..5
    if (op <= 0x0a) { out = {0, 2}; return true; }              // lconst
    if (op <= 0x0d) { out = {0, 1}; return true; }              // fconst
    if (op <= 0x0f) { out = {0, 2}; return true; }              // dconst
    if (op <= 0x11) { out = {0, 1}; return true; }              // bipush/sipush
    if (op <= 0x13) { out = {0, 1}; return true; }              // ldc/ldc_w
    if (op == 0x14) { out = {0, 2}; return true; }              // ldc2_w
    if (op <= 0x19) { out = {0, (op == 0x16 || op == 0x18) ? 2u : 1u}; return true; }
    if (op <= 0x1d) { out = {0, 1}; return true; }              // iload_n
    if (op <= 0x21) { out = {0, 2}; return true; }              // lload_n
    if (op <= 0x25) { out = {0, 1}; return true; }              // fload_n
    if (op <= 0x29) { out = {0, 2}; return true; }              // dload_n
    if (op <= 0x2d) { out = {0, 1}; return true; }              // aload_n（0x2a-0x2d 全 1 槽）
    if (op <= 0x35) {                                           // xaload
        out = {2, (op == 0x2f || op == 0x31) ? 2u : 1u};        // laload/daload 双槽
        return true;
    }
    if (op <= 0x3a) { out = {(op == 0x37 || op == 0x39) ? 2u : 1u, 0}; return true; }
    if (op <= 0x3e) { out = {1, 0}; return true; }              // istore_n
    if (op <= 0x42) { out = {2, 0}; return true; }              // lstore_n
    if (op <= 0x46) { out = {1, 0}; return true; }              // fstore_n
    if (op <= 0x4a) { out = {2, 0}; return true; }              // dstore_n
    if (op <= 0x4e) { out = {1, 0}; return true; }              // astore_n
    if (op <= 0x56) { out = {(op == 0x50 || op == 0x52) ? 4u : 3u, 0}; return true; }
    switch (op) {                                               // 栈操作
    case 0x57: out = {1, 0}; return true;    // pop
    case 0x58: out = {2, 0}; return true;    // pop2
    case 0x59: out = {1, 2}; return true;    // dup
    case 0x5a: out = {2, 4}; return true;    // dup2
    case 0x5b: out = {2, 3}; return true;    // dup_x1
    case 0x5c: out = {3, 4}; return true;    // dup_x2
    case 0x5d: out = {3, 5}; return true;    // dup2_x1
    case 0x5e: out = {4, 6}; return true;    // dup2_x2
    case 0x5f: out = {2, 2}; return true;    // swap
    default: break;
    }
    if (op == 0x84) { out = {0, 0}; return true; }              // iinc（不动栈）
    if (op <= 0x83) {                                           // 算术
        if (op == 0x79 || op == 0x7b || op == 0x7d) {
            out = {3, 2};
            return true;                                         // lshl/lshr/lushr
        }
        if (op == 0x61 || op == 0x65 || op == 0x69 || op == 0x6d || op == 0x71
            || op == 0x7f || op == 0x81 || op == 0x83) {
            out = {4, 2};
            return true;
        }
        if (op == 0x75 || op == 0x77) {
            out = {2, 2};
            return true;                                         // lneg/dneg
        }
        if (op == 0x74 || op == 0x76) {
            out = {1, 1};
            return true;                                         // ineg/fneg
        }
        out = {2, 1};
        return true;
    }
    switch (op) {                                               // 转换/比较
    case 0x85: out = {1, 2}; return true;    // i2l
    case 0x86: out = {1, 1}; return true;    // i2f
    case 0x87: out = {1, 2}; return true;    // i2d
    case 0x88: out = {2, 1}; return true;    // l2i
    case 0x89: out = {2, 1}; return true;    // l2f
    case 0x8a: out = {2, 2}; return true;    // l2d
    case 0x8b: out = {1, 1}; return true;    // f2i
    case 0x8c: out = {1, 2}; return true;    // f2l
    case 0x8d: out = {1, 2}; return true;    // f2d
    case 0x8e: out = {2, 1}; return true;    // d2i
    case 0x8f: out = {2, 2}; return true;    // d2l
    case 0x90: out = {2, 1}; return true;    // d2f
    case 0x91: case 0x92: case 0x93: out = {1, 1}; return true;   // i2b/c/s
    case 0x94: out = {4, 1}; return true;    // lcmp
    case 0x95: case 0x96: out = {2, 1}; return true;    // fcmpl/g
    case 0x97: case 0x98: out = {4, 1}; return true;    // dcmpl/g
    default: break;
    }
    if (op <= 0x9e) { out = {1, 0}; return true; }              // if*
    if (op <= 0xa6) { out = {2, 0}; return true; }              // if_icmp*/if_acmp*
    if (op == 0xa7) { out = {0, 0}; return true; }              // goto
    if (op == 0xa8) { out = {0, 1}; return true; }              // jsr（白名单外）
    if (op == 0xa9) { out = {1, 0}; return true; }              // ret（白名单外）
    if (op == 0xaa || op == 0xab) { out = {1, 0}; return true; } // switch 弹 key
    if (op == 0xac || op == 0xae || op == 0xb0) { out = {1, 0}; return true; }
    if (op == 0xad || op == 0xaf) { out = {2, 0}; return true; }
    if (op == 0xb1) { out = {0, 0}; return true; }
    if (op == 0xb2) {                                           // getstatic
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        out = {0, fieldSlots(r.desc)};
        return true;
    }
    if (op == 0xb3) {                                           // putstatic
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        out = {fieldSlots(r.desc), 0};
        return true;
    }
    if (op == 0xb4) {                                           // getfield
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        out = {1, fieldSlots(r.desc)};
        return true;
    }
    if (op == 0xb5) {                                           // putfield
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        out = {1 + fieldSlots(r.desc), 0};
        return true;
    }
    if (op >= 0xb6 && op <= 0xba) {                             // 调用族
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        const bool recv = (op == 0xb6 || op == 0xb7 || op == 0xb9);
        out = {descArgSlots(r.desc) + (recv ? 1u : 0u), descRetSlots(r.desc)};
        return true;
    }
    if (op == 0xbb || op == 0xbc || op == 0xbd || op == 0xbe) {
        out = {op == 0xbb ? 0u : 1u, 1};
        return true;
    }
    if (op == 0xbf) { out = {1, 0}; return true; }              // athrow
    if (op == 0xc0 || op == 0xc1) { out = {1, 1}; return true; }
    if (op == 0xc2 || op == 0xc3) { out = {1, 0}; return true; }
    if (op == 0xc5) { out = {1, 1}; return true; }              // multianewarray 近似
    if (op == 0xc6 || op == 0xc7) { out = {1, 0}; return true; }
    if (op == 0xc8) { out = {0, 0}; return true; }
    return false;
}

/// @brief 容器内剩余 junk 形成员计数（residual 记账）。
void countResidual(const mjbase::model::ResourceContainer& container,
                   ca::usize& methods, ca::usize& fields)
{
    methods = 0;
    fields  = 0;
    pass_clock::Suspend suspend;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        const jbc::core::ClassFile& cf = session->backing();
        for (const jbc::core::MemberInfo& m : cf.methods) {
            if (junkMethodName(cf.utf8(m.nameCp))) ++methods;
        }
        for (const jbc::core::MemberInfo& f : cf.fields) {
            if (junkFieldName(cf.utf8(f.nameCp))) ++fields;
        }
    });
}

// ---------------------------------------------------------------------------
// 白名单判定
// ---------------------------------------------------------------------------

/// @brief 引用是否指向本类 junk 形成员。
bool isJunkFieldRef(const jbc::core::ClassFile& cf, const std::string& owner,
                    const std::string& name)
{
    return owner == std::string(cf.thisName()) && junkFieldName(name);
}

bool isJunkMethodRef(const jbc::core::ClassFile& cf, const std::string& owner,
                     const std::string& name)
{
    return owner == std::string(cf.thisName()) && junkMethodName(name);
}

/// @brief 一条指令的白名单资格（tier-1 + tier-2 诱饵吸收）。
///
/// @param nextOp 后继指令操作码（tier-2 的立即 pop 判据；越界传 0x100）。
bool insnEligible(const jbc::core::ClassFile& cf, const Insn& in, ca::u32 nextOp)
{
    const ca::u32 op = in.op;
    if (op <= 0x14) {                              // nop..ldc2_w
        if (op == 0x12 || op == 0x13 || op == 0x14) return cpIsNumericConst(cf, in.cp);
        return true;                               // nop/常量/bipush/sipush
    }
    if ((op >= 0x15 && op <= 0x2d) || (op >= 0x36 && op <= 0x4e)) return true;
    if (op >= 0x57 && op <= 0x98) return true;     // 栈操作/算术/转换/比较/iinc
    if ((op >= 0x99 && op <= 0xa8) || op == 0xc6 || op == 0xc7 || op == 0xc8) return true;
    if (op == 0xaa || op == 0xab) return true;     // switch（目标在闭包内由闭包判定）
    if (op == 0xb1 || op == 0xbf) return true;     // return / athrow（陷阱拆除）
    if (op == 0xb2 || op == 0xb3 || op == 0xb8) {
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        if (op == 0xb8) return isJunkMethodRef(cf, r.owner, r.name);
        if (isJunkFieldRef(cf, r.owner, r.name)) return true;
        // tier-2：真实成员读/调且值被立即丢弃（诱饵 trampoline，普查背书）。
        return nextOp == 0x57 || nextOp == 0x58;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 栈深模拟（原方法与改写后共用；masked 非空时闭包指令按 NOP 直落）
// ---------------------------------------------------------------------------

struct SimOutcome
{
    std::map<ca::u32, ca::u32> depth;   ///< pc → 入口槽深（可达面）
    bool                       ok = false;
    std::string                reason;  ///< 失败原因（trace 用）
    ca::u32                    failPc = 0;
};

SimOutcome simulate(const jbc::core::ClassFile& cf, const Body& body,
                    const std::vector<ca::u8>& code, const std::set<ca::usize>* masked)
{
    SimOutcome out;
    if (!body.ok) {
        out.reason = "decode";
        return out;
    }
    const ca::u32 codeLen = static_cast<ca::u32>(code.size());
    std::vector<std::pair<ca::u32, ca::u32>> work;
    work.emplace_back(0, 0);
    for (const ca::u32 h : body.handlers) {
        work.emplace_back(h, 1);
    }
    while (!work.empty()) {
        const auto [pc, d] = work.back();
        work.pop_back();
        if (pc >= codeLen) {
            out.reason = "off-end";
            out.failPc = pc;
            return out;
        }
        const auto it = body.byPc.find(pc);
        if (it == body.byPc.end()) {
            out.reason = "no-insn";
            out.failPc = pc;
            return out;
        }
        const ca::usize idx = it->second;
        const auto hit = out.depth.find(pc);
        if (hit != out.depth.end()) {
            if (hit->second != d) {
                out.reason = "mismatch";
                out.failPc = pc;
                return out;
            }
            continue;
        }
        out.depth.emplace(pc, d);
        const Insn& in = body.insns[idx];
        const bool maskedInsn = masked != nullptr && masked->count(idx) != 0;
        ca::u32 nd = d;
        std::vector<ca::u32> succ;
        if (maskedInsn) {
            succ.push_back(in.pc + in.len);        // NOP 直落
        } else {
            Eff eff;
            if (!slotEffect(cf, in, eff)) {
                out.reason = "unknown-eff";
                out.failPc = pc;
                return out;
            }
            if (d < eff.pop) {
                out.reason = "underflow";
                out.failPc = pc;
                return out;
            }
            nd = d - eff.pop + eff.push;
            succ = successors(body, idx);
        }
        for (const ca::u32 t : succ) {
            if (t >= codeLen) {
                out.reason = "off-end";
                out.failPc = t;
                return out;
            }
            work.emplace_back(t, nd);
        }
    }
    out.ok = true;
    return out;
}

/// @brief 诱饵形指令（孤儿容许的内容门）。
bool decoyShaped(const jbc::core::ClassFile& cf, const Insn& in)
{
    const ca::u32 op = in.op;
    if (op == 0xbb || op == 0xbc || op == 0xbd || op == 0xc5) return false;   // new 族
    if (op == 0xb7) return false;                                             // invokespecial
    if (op == 0xc0 || op == 0xc1) return false;                               // checkcast/instanceof
    if (op == 0xc2 || op == 0xc3 || op == 0xc4 || op == 0xba) return false;   // monitor/wide/indy
    if (op == 0xb5) return false;                                             // putfield
    if (op == 0xb3) {                                                         // putstatic 仅 junk 字段
        Ref r;
        if (!resolveRef(cf, in.cp, r)) return false;
        return isJunkFieldRef(cf, r.owner, r.name);
    }
    if (op >= 0x4f && op <= 0x56) return false;                               // xastore
    if (op >= 0x15 && op <= 0x2d) return false;                               // 局部读（this/参数面）
    if (op == 0x12 || op == 0x13) return cpIsNumericConst(cf, in.cp);         // ldc 仅数值
    return true;
}

/// @brief 一个 SMT 帧的改写核对面：绝对 pc / 帧型 / 声明栈槽数 / locals 增量
///        / 改帧后的 locals 内容（验证标签序列，count 与 Top 半槽都含）。
struct SmtFrameInfo
{
    ca::u32 pc    = 0;
    ca::u8  type  = 0;    ///< 帧形态标签（JVMS 4.7.4）
    ca::u32 stack = 0;    ///< 声明栈槽（same 0 / same1 1 / full = stack.size()）
    ca::i64 localsDelta = 0;   ///< 该帧导致的 locals 计数变化（append/chop/full）
    std::vector<jbc::core::StackMapTableAttr::VType> localsTags;   ///< 改帧后的 locals 内容
};

/// @brief 描述符参数项的验证标签序列（locals 内容比对用；ref 系一律 Object
///        标签，long/double 占双槽、半槽为 Top）。
std::vector<jbc::core::StackMapTableAttr::VType> descParamTags(std::string_view desc)
{
    using jbc::core::StackMapTableAttr;
    std::vector<jbc::core::StackMapTableAttr::VType> out;
    ca::usize i = 1;   // 跳过 '('
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        if (c == 'L') {
            while (i < desc.size() && desc[i] != ';') ++i;
            ++i;
            out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kObject, 0});
        } else if (c == '[') {
            while (i < desc.size() && desc[i] == '[') ++i;
            if (i < desc.size() && desc[i] == 'L') {
                while (i < desc.size() && desc[i] != ';') ++i;
            }
            ++i;
            out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kObject, 0});
        } else {
            switch (c) {
            case 'J': out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kLong, 0});
                      out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kTop, 0}); break;
            case 'D': out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kDouble, 0});
                      out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kTop, 0}); break;
            case 'F': out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kFloat, 0}); break;
            default: out.push_back(StackMapTableAttr::VType{StackMapTableAttr::VType::kInteger, 0}); break;
            }
            ++i;
        }
    }
    return out;
}

/// @brief 方法的初始帧 locals 内容（<init> 的 this 为 uninitializedThis，
///        其余实例方法为 Object，静态方法无收者；参数项按描述符展开）。
std::vector<jbc::core::StackMapTableAttr::VType> initialLocalsContent(
    const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& m)
{
    using jbc::core::StackMapTableAttr;
    std::vector<jbc::core::StackMapTableAttr::VType> out;
    const bool isStatic = (m.access & 0x0008) != 0;
    const std::string_view name = cf.utf8(m.nameCp);
    if (!isStatic) {
        out.push_back(name == "<init>"
                          ? StackMapTableAttr::VType{StackMapTableAttr::VType::kUninitializedThis, 0}
                          : StackMapTableAttr::VType{StackMapTableAttr::VType::kObject, 0});
    }
    const auto params = descParamTags(cf.utf8(m.descCp));
    out.insert(out.end(), params.begin(), params.end());
    return out;
}

/// @brief SMT 帧信息集（绝对 pc / 帧型 / 声明栈深 / locals 内容）；解不开返回 false。
bool smtFrames(jbc::core::ClassFile& cf, jbc::core::CodeAttr& body,
               const std::vector<jbc::core::StackMapTableAttr::VType>& initialLocals,
               std::vector<SmtFrameInfo>& out)
{
    using jbc::core::StackMapTableAttr;
    for (jbc::core::AttrSlot& slot : body.attrs) {
        if (cf.utf8(slot.nameCp) != "StackMapTable") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        const auto* smt = ca::core::tag_cast::dyn_cast<StackMapTableAttr>(d.unwrap());
        if (smt == nullptr) return false;
        ca::i64 prev = -1;
        std::vector<jbc::core::StackMapTableAttr::VType> locals = initialLocals;
        for (const StackMapTableAttr::Frame& f : smt->frames) {
            ca::u32 delta = 0;
            ca::u32 stack = 0;
            ca::i64 newLocals = static_cast<ca::i64>(locals.size());
            if (f.type <= StackMapTableAttr::Frame::kSameMax) {
                delta = f.type;                                   // same_frame
            } else if (f.type >= StackMapTableAttr::Frame::kSame1Base
                       && f.type <= StackMapTableAttr::Frame::kSame1Max) {
                delta = f.type - StackMapTableAttr::Frame::kSame1Base;
                stack = 1;                                        // same_locals_1_stack_item
            } else if (f.type == StackMapTableAttr::Frame::kSame1Ext) {
                delta = f.offset;
                stack = 1;
            } else if (f.type >= StackMapTableAttr::Frame::kChopMax
                       && f.type <= StackMapTableAttr::Frame::kChopMin) {
                delta = f.offset;
                const ca::i64 k = StackMapTableAttr::Frame::kChopMin + 1
                                  - static_cast<ca::i64>(f.type);
                newLocals -= k;
                if (newLocals < 0) return false;
                locals.resize(static_cast<ca::usize>(newLocals));
            } else if (f.type == StackMapTableAttr::Frame::kSameExt) {
                delta = f.offset;
            } else if (f.type >= StackMapTableAttr::Frame::kAppendMin
                       && f.type <= StackMapTableAttr::Frame::kAppendMax) {
                delta = f.offset;
                locals.insert(locals.end(), f.locals.begin(), f.locals.end());
            } else if (f.type == StackMapTableAttr::Frame::kFull) {
                delta = f.offset;
                stack = static_cast<ca::u32>(f.stack.size());
                locals.assign(f.locals.begin(), f.locals.end());
            } else {
                return false;   ///< 128..246 保留标签：不猜
            }
            const ca::u32 pc = (prev < 0)
                                   ? delta
                                   : static_cast<ca::u32>(prev) + delta + 1;
            out.push_back({pc, f.type, stack,
                           static_cast<ca::i64>(locals.size()) - newLocals, locals});
            prev = pc;
        }
    }
    return true;
}

/// @brief 单方法的区域判定（scan 本体）。@return false = 方法整条放弃。
/// 最近一次整方法放弃的判据位置（GRUNT_JMS_TRACE 诊断用；单线程进程内）。
std::string g_lastAbandon;

bool detectRegion(jbc::core::ClassFile& cf, const std::vector<ca::u8>& code,
                  jbc::core::CodeAttr& attr, JunkRegion& out,
                  ca::usize methodIndex, ScanResult& stats)
{
    g_lastAbandon.clear();
    Body bodyDecoded = decodeBody(code);
    for (const jbc::core::CodeAttr::ExcEntry& e : attr.excTable) {
        bodyDecoded.handlers.push_back(e.handlerPc);
    }
    // (1) 原方法栈深模拟（入口 0 + handler 1）
    const SimOutcome pre = simulate(cf, bodyDecoded, code, nullptr);
    if (!pre.ok) {
        const auto hit = bodyDecoded.byPc.find(pre.failPc);
        const ca::u32 op = (hit != bodyDecoded.byPc.end())
                               ? bodyDecoded.insns[hit->second].op
                               : static_cast<ca::u32>(-1);
        g_lastAbandon = "sim:" + pre.reason + "@pc" + std::to_string(pre.failPc)
                        + " op" + std::to_string(op);
        ++stats.methodsSimFail;
        return false;
    }
    const ca::usize n = bodyDecoded.insns.size();
    // (2) 逐指令白名单资格（tier-2 看后继）
    std::vector<bool> elig(n, false);
    for (ca::usize i = 0; i < n; ++i) {
        const Insn& in = bodyDecoded.insns[i];
        const ca::u32 nextOp = (i + 1 < n) ? bodyDecoded.insns[i + 1].op : 0x100;
        elig[i] = insnEligible(cf, in, nextOp);
    }
    // (3) 种子 = junk 形成员引用
    std::set<ca::usize> seeds;
    for (ca::usize i = 0; i < n; ++i) {
        const Insn& in = bodyDecoded.insns[i];
        if (in.op != 0xb2 && in.op != 0xb3 && in.op != 0xb8) continue;
        Ref r;
        if (!resolveRef(cf, in.cp, r)) continue;
        const bool seed = (in.op == 0xb8)
                              ? isJunkMethodRef(cf, r.owner, r.name)
                              : isJunkFieldRef(cf, r.owner, r.name);
        if (seed) seeds.insert(i);
    }
    if (seeds.empty()) return true;    // 无 junk 引用：非 junk 方法，不动
    // (G3) 区域须有 ≥1 junk 字段种子：junk 键状态机的结构证据（普查背书：
    // 真实产物的每段区域都在操作本类 junk 字段）。纯探针调用点（真实方法
    // invokestatic 一个 junk 方法）不构成区域——真实调用者的方法体照原样
    // 保真，探针方法由引用普查关死删除面。
    bool fieldSeed = false;
    for (const ca::usize i : seeds) {
        if (bodyDecoded.insns[i].op != 0xb8) fieldSeed = true;
    }
    if (!fieldSeed) return true;
    stats.seedsTotal += static_cast<ca::usize>(seeds.size());
    // (4) junk CFG 闭包：种子沿 CFG 边只在白名单指令上扩张
    std::set<ca::usize> closure = seeds;
    std::vector<ca::usize> work(seeds.begin(), seeds.end());
    while (!work.empty()) {
        const ca::usize i = work.back();
        work.pop_back();
        for (const ca::u32 t : successors(bodyDecoded, i)) {
            if (t >= code.size()) continue;
            const auto hit = bodyDecoded.byPc.find(t);
            if (hit == bodyDecoded.byPc.end()) continue;
            const ca::usize ti = hit->second;
            if (!elig[ti] || closure.count(ti) != 0) continue;
            closure.insert(ti);
            work.push_back(ti);
        }
    }
    // (4b) 值馈送回拉：闭包成员的**直落前驱**（白名单内）回拉——putstatic
    // 种子消费的 ldc2_w/nop 前缀在正向闭包上游；回拉后重新正向扩张（分支
    // 边），到不动点为止。真实锚（new/真实字段写/真实调用）天然挡住回拉链。
    {
        std::unordered_map<ca::u32, ca::usize> fallPred;
        for (ca::usize i = 0; i < n; ++i) {
            fallPred.emplace(bodyDecoded.insns[i].pc + bodyDecoded.insns[i].len, i);
        }
        bool grew = true;
        while (grew) {
            grew = false;
            std::vector<ca::usize> all(closure.begin(), closure.end());
            for (const ca::usize i : all) {
                const auto it = fallPred.find(bodyDecoded.insns[i].pc);
                if (it == fallPred.end()) continue;
                // (G4) 链式回拉：从闭包成员沿直落前驱走到链头（锚/边界），
                // **整链净槽位效应 ≥ 0 才整链收**——链不许消费闭包外的栈值
                // （锚推上来、pop 收尾的链一旦被 NOP 化，锚的值会漏进保留尾
                // return，改写后槽深非零 → 整方法放弃）。junk 值馈送链
                // （ldc2_w/nop 对，净 +2）与纯 nop 段（净 0）都过闸；putstatic
                // 种子上游整链（净 0）也过闸。
                std::vector<ca::usize> chain;
                const ca::usize   head = it->second;
                ca::usize         walk = head;
                ca::i64           net  = 0;
                while (true) {
                    if (!elig[walk]) break;
                    if (closure.count(walk) != 0) break;   // 已是闭包成员：链到此为止
                    Eff eff;
                    if (!slotEffect(cf, bodyDecoded.insns[walk], eff)) break;
                    net += static_cast<ca::i64>(eff.push)
                           - static_cast<ca::i64>(eff.pop);
                    chain.push_back(walk);
                    const auto pit = fallPred.find(bodyDecoded.insns[walk].pc);
                    if (pit == fallPred.end()) break;
                    walk = pit->second;
                }
                if (net < 0) continue;
                for (const ca::usize ci : chain) {
                    if (closure.insert(ci).second) grew = true;
                }
            }
            for (const ca::usize i : all) {
                for (const ca::u32 t : successors(bodyDecoded, i)) {
                    if (t >= code.size()) continue;
                    const auto hit = bodyDecoded.byPc.find(t);
                    if (hit == bodyDecoded.byPc.end()) continue;
                    const ca::usize ti = hit->second;
                    if (!elig[ti] || closure.count(ti) != 0) continue;
                    closure.insert(ti);
                    grew = true;
                }
            }
        }
    }
    // (5) 保留尾 return：闭包内最后一个 `return` 不 NOP（后缀区域唯一出口）
    ca::usize keptIdx = static_cast<ca::usize>(-1);
    for (ca::usize i = 0; i < n; ++i) {
        if (closure.count(i) != 0 && bodyDecoded.insns[i].op == 0xb1) keptIdx = i;
    }
    // (6) 前沿收缩：改写后模拟失败时把喂值闭包指令退出（≤64 轮）
    std::set<ca::usize> mask = closure;
    if (keptIdx != static_cast<ca::usize>(-1)) mask.erase(keptIdx);
    SimOutcome post;
    ca::usize shrinks = 0;
    while (true) {
        post = simulate(cf, bodyDecoded, code, &mask);
        if (post.ok) break;
        if (post.reason != "underflow" && post.reason != "mismatch") break;
        const auto hit = bodyDecoded.byPc.find(post.failPc);
        if (hit == bodyDecoded.byPc.end()) break;
        const ca::usize failIdx = hit->second;
        std::vector<ca::usize> feeders;
        for (const ca::usize i : mask) {
            if (seeds.count(i) != 0) continue;
            for (const ca::u32 t : successors(bodyDecoded, i)) {
                if (t == post.failPc) {
                    feeders.push_back(i);
                    break;
                }
            }
        }
        if (feeders.empty()) break;
        for (const ca::usize i : feeders) {
            mask.erase(i);
            closure.erase(i);
        }
        if (keptIdx != static_cast<ca::usize>(-1) && closure.count(keptIdx) == 0) {
            keptIdx = static_cast<ca::usize>(-1);
        }
        if (++shrinks > 64) break;
    }
    if (!post.ok) {
        g_lastAbandon = "rewrite-sim:" + post.reason + "@pc" + std::to_string(post.failPc);
        ++stats.methodsFailClosed;
        return false;
    }
    // (G2) 区域不得从方法入口起（须有真实前缀）：真实方法整段落在白名单内
    // 时（纯算术体 + 探针调用），闭包会覆盖全方法——那不是 junk 区域，NOP 化
    // 会把真实语义抹掉。真实产物的区域起点全部在真实初始化前缀之后（普查：
    // clinit 11 / init 4-5 起）。
    if (!closure.empty() && bodyDecoded.insns[*closure.begin()].pc == 0) {
        g_lastAbandon = "G2-entry-region";
        ++stats.methodsFailClosed;
        return false;
    }
    // (7) 孤儿容许：改写后不可达的指令须整集诱饵形（真实初始化签名即放弃）
    {
        std::vector<ca::usize> lost;
        for (ca::usize i = 0; i < n; ++i) {
            const ca::u32 pc = bodyDecoded.insns[i].pc;
            if (closure.count(i) != 0) continue;
            if (pre.depth.count(pc) == 0) continue;
            if (post.depth.count(pc) != 0) continue;
            lost.push_back(i);
        }
        if (!lost.empty()) {
            for (const ca::usize i : lost) {
                if (!decoyShaped(cf, bodyDecoded.insns[i])) {
                    g_lastAbandon = "lost-nondecoy@pc"
                                    + std::to_string(bodyDecoded.insns[i].pc) + " op0x"
                                    + std::to_string(bodyDecoded.insns[i].op);
                    ++stats.methodsFailClosed;
                    return false;
                }
            }
            stats.orphanInsns += lost.size();
        }
    }
    // (8) 夹层真实块 ≤ 1（改写后执行序由 pc 序决定，多块原序不可证）
    if (!closure.empty()) {
        const ca::u32 minPc = bodyDecoded.insns[*closure.begin()].pc;
        ca::u32 maxPc = 0;
        for (const ca::usize i : closure) {
            maxPc = std::max(maxPc, bodyDecoded.insns[i].pc);
        }
        ca::u32 blocks     = 0;
        bool    inBlock    = false;
        for (ca::usize i = 0; i < n; ++i) {
            const ca::u32 pc = bodyDecoded.insns[i].pc;
            if (pc <= minPc || pc >= maxPc) {
                inBlock = false;
                continue;
            }
            if (closure.count(i) == 0) {
                if (!inBlock) {
                    ++blocks;
                    inBlock = true;
                }
            } else {
                inBlock = false;
            }
        }
        if (blocks > 1) {
            g_lastAbandon = "multi-blocks";
            ++stats.methodsFailClosed;
            return false;
        }
    }
    // (9) 异常 handler 不得落在闭包内（handler 入口帧锚不可丢）
    for (const ca::u32 h : bodyDecoded.handlers) {
        const auto hit = bodyDecoded.byPc.find(h);
        if (hit != bodyDecoded.byPc.end() && closure.count(hit->second) != 0) {
            g_lastAbandon = "handler-in-closure";
            ++stats.methodsFailClosed;
            return false;
        }
    }
    // (10) SMT：帧信息（绝对 pc / 帧型 / 声明栈深 / locals 内容）；解不开整
    //      方法放弃
    const auto initLocals = initialLocalsContent(cf, cf.methods[methodIndex]);
    std::vector<SmtFrameInfo> frames;
    if (!smtFrames(cf, attr, initLocals, frames)) {
        g_lastAbandon = "smt-undecodable";
        ++stats.methodsFailClosed;
        return false;
    }
    // (11) 保留尾 return 的改写后槽深必须为 0（return 要求空栈）
    if (keptIdx != static_cast<ca::usize>(-1)) {
        const auto hit = post.depth.find(bodyDecoded.insns[keptIdx].pc);
        if (hit == post.depth.end() || hit->second != 0) {
            g_lastAbandon = "kept-return-depth:"
                            + std::to_string(hit == post.depth.end() ? -1 : hit->second);
            ++stats.methodsFailClosed;
            return false;
        }
    }
    // (12) 造段：闭包指令 pc 区间并段；**保留尾 return 及其后的陷阱尾不在段
    //      内**——陷阱尾（pop/goto 回跳保留尾的蹦床，原指令流）保持原样是合
    //      法的迂回出口：其 SMT 帧声明状态对原指令流成立，locals 基线（构造
    //      器的 initialized-this 等）不被摘帧破坏，也不产生直落出代码末。
    std::vector<std::pair<ca::u32, ca::u32>> raw;
    ca::usize athrows = 0;
    const ca::u32 keptReturnPc = (keptIdx == static_cast<ca::usize>(-1))
                                     ? static_cast<ca::u32>(code.size())
                                     : bodyDecoded.insns[keptIdx].pc;
    for (const ca::usize i : closure) {
        const Insn& in = bodyDecoded.insns[i];
        if (in.pc >= keptReturnPc) continue;   // 保留尾 + 陷阱尾不 NOP
        raw.emplace_back(in.pc, in.pc + in.len);
        if (in.op == 0xbf) ++athrows;
    }
    std::sort(raw.begin(), raw.end());
    std::vector<std::pair<ca::u32, ca::u32>> spans;
    for (const auto& r : raw) {
        if (!spans.empty() && r.first <= spans.back().second) {
            spans.back().second = std::max(spans.back().second, r.second);
        } else {
            spans.push_back(r);
        }
    }
    // (F1) 摘除集 = 锚在 NOP 化段内 pc 的帧；陷阱尾/保留尾的帧原样保留——
    //      它们的声明状态对未改动的指令流成立。段内 **full 帧**若声明态与改
    //      写后计算态一致（栈深一致 + locals 内容 == 初始内容 + 其前无掩蔽段
    //      外的真实 locals 写）则保留不摘：它携带 locals 类型态（构造器的
    //      initialized-this 转变正由它承载，实证），摘掉会让其后保留帧的差分
    //      基线塌回 uninitializedThis（"Instruction type does not match stack
    //      map" 实证）；栈形/locals 对不上才摘（核对交 (F2)/(F3) 面）。
    std::vector<ca::u32> dropPcs;
    for (const SmtFrameInfo& f : frames) {
        bool inSpan = false;
        for (const auto& [start, end] : spans) {
            if (f.pc >= start && f.pc < end) {
                inSpan = true;
                break;
            }
        }
        if (!inSpan) continue;
        if (f.type == jbc::core::StackMapTableAttr::Frame::kFull) {
            const auto hit = post.depth.find(f.pc);
            if (hit != post.depth.end() && hit->second == f.stack
                && f.localsTags == initLocals) {
                // 其前不得有掩蔽段外的真实 locals 写（store/iinc）——写了则
                // 计算态 locals 偏离初始内容，帧声明必不符
                bool polluted = false;
                for (ca::usize i2 = 0; i2 < n; ++i2) {
                    const Insn& in2 = bodyDecoded.insns[i2];
                    if (in2.pc >= f.pc) break;
                    bool inSpan2 = false;
                    for (const auto& [start, end] : spans) {
                        inSpan2 |= (in2.pc >= start && in2.pc < end);
                    }
                    if (inSpan2 || in2.pc >= keptReturnPc) continue;
                    if ((in2.op >= 0x36 && in2.op <= 0x4e) || in2.op == 0x84) {
                        polluted = true;
                        break;
                    }
                }
                if (!polluted) continue;
            }
        }
        dropPcs.push_back(f.pc);
    }
    // (F2) 保留帧核对：改写后可达的保留帧，声明栈深必须与改写后槽深一致
    //      （NOP 化抹掉探针值后，真实块入口的帧若还声明旧栈形，校验器必报
    //      Inconsistent stackmap；宁整方法放弃）。不可达帧无入边，不核对。
    for (const SmtFrameInfo& f : frames) {
        bool dropped = false;
        for (const ca::u32 pc : dropPcs) dropped |= (pc == f.pc);
        if (dropped) continue;
        const auto hit = post.depth.find(f.pc);
        if (hit == post.depth.end()) continue;
        if (hit->second != f.stack) {
            g_lastAbandon = "frame-stack-mismatch@pc" + std::to_string(f.pc)
                            + " decl" + std::to_string(f.stack) + " post"
                            + std::to_string(hit->second);
            ++stats.methodsFailClosed;
            return false;
        }
    }
    // (F3) locals 基线：被摘帧若携带 locals 态变化，其后改写后可达保留帧的
    //      差分基线偏离真态，校验器必错 → 整方法放弃。真态（orig，沿原链推
    //      进）与保留链基线（keptBase，仅保留帧重置/增删）逐可达保留帧对照；
    //      内容与真态一致的被摘帧不伤基线。
    {
        std::vector<jbc::core::StackMapTableAttr::VType> orig    = initLocals;
        std::vector<jbc::core::StackMapTableAttr::VType> keptBase = initLocals;
        bool dirty = false;
        for (const SmtFrameInfo& f : frames) {
            using FrameT = jbc::core::StackMapTableAttr::Frame;
            const bool full   = (f.type == FrameT::kFull);
            const bool append = (f.type >= FrameT::kAppendMin
                                 && f.type <= FrameT::kAppendMax);
            const bool chop   = (f.type >= FrameT::kChopMax
                                 && f.type <= FrameT::kChopMin);
            bool dropped = false;
            for (const ca::u32 pc : dropPcs) dropped |= (pc == f.pc);
            // 真态推进（含被摘帧）
            if (full) {
                orig.assign(f.localsTags.begin(), f.localsTags.end());
            } else if (append) {
                orig.insert(orig.end(), f.localsTags.end()
                                            - static_cast<ca::usize>(f.localsDelta),
                                            f.localsTags.end());
            } else if (chop) {
                orig.resize(static_cast<ca::usize>(
                    static_cast<ca::i64>(orig.size()) + f.localsDelta));
            }
            if (!dropped) {
                // 保留链基线推进
                if (full) {
                    keptBase.assign(f.localsTags.begin(), f.localsTags.end());
                } else if (append) {
                    keptBase.insert(keptBase.end(),
                                    f.localsTags.end()
                                        - static_cast<ca::usize>(f.localsDelta),
                                    f.localsTags.end());
                } else if (chop) {
                    keptBase.resize(static_cast<ca::usize>(
                        static_cast<ca::i64>(keptBase.size()) + f.localsDelta));
                }
                if (post.depth.count(f.pc) != 0 && orig != keptBase) {
                    dirty = true;
                    break;
                }
            }
        }
        if (dirty) {
            g_lastAbandon = "frame-locals-baseline";
            ++stats.methodsFailClosed;
            return false;
        }
    }
    // (F4) 真实分支闸：改写后仍在分支的指令（掩蔽段外——含陷阱尾的真实分
    //      支）不得指向**帧被摘**的 pc（分支目标无帧校验器必报错）；指向保
    //      留尾/陷阱尾（帧保留）的分支合法——陷阱蹦床回跳保留尾正是这样的边。
    for (ca::usize i = 0; i < n; ++i) {
        const ca::u32 bpc = bodyDecoded.insns[i].pc;
        bool branchMasked = false;
        for (const auto& [start, end] : spans) branchMasked |= (bpc >= start && bpc < end);
        if (branchMasked) continue;
        const ca::u32 op = bodyDecoded.insns[i].op;
        const bool isBranch = (op >= 0x99 && op <= 0xa8) || op == 0xaa || op == 0xab
                              || op == 0xc6 || op == 0xc7 || op == 0xc8;
        if (!isBranch) continue;
        for (const ca::u32 t : successors(bodyDecoded, i)) {
            const auto hit = bodyDecoded.byPc.find(t);
            if (hit == bodyDecoded.byPc.end()) continue;
            bool targetDropped = false;
            for (const ca::u32 pc : dropPcs) targetDropped |= (pc == t);
            if (targetDropped) {
                g_lastAbandon = "real-branch-into-dropped-frame@pc"
                                + std::to_string(bodyDecoded.insns[i].pc);
                ++stats.methodsFailClosed;
                return false;
            }
        }
    }
    out.methodIndex = methodIndex;
    out.spans = std::move(spans);
    out.droppedFramePcs = std::move(dropPcs);
    out.athrowCount = athrows;
    ++stats.regionsTotal;
    return true;
}

/// @brief 一类的候选收集（SCAN 本体）。
ClassCand scanClass(ir::ClassSession& s, const std::string& uri, ScanResult& stats)
{
    static const bool trace = std::getenv("GRUNT_JMS_TRACE") != nullptr;
    ClassCand             cand;
    cand.uri   = uri;
    jbc::core::ClassFile& cf = s.backing();
    cand.owner = std::string(cf.thisName());
    // junk 形成员普查（名字形；删除面在 rewrite 由引用普查关死）
    for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
        if (junkFieldName(cf.utf8(cf.fields[fi].nameCp))) cand.fieldIdxs.push_back(fi);
    }
    for (ca::usize mi = 0; mi < cf.methods.size(); ++mi) {
        if (!junkMethodName(cf.utf8(cf.methods[mi].nameCp))) continue;
        cand.methodIdxs.push_back(mi);
    }
    if (cand.methodIdxs.empty() && cand.fieldIdxs.empty()) return cand;
    // 逐方法区域判定（G1：junk 命名的方法只作删除候选，不做区域宿主——
    // 探针体内也有 junk 字段引用，NOP 化探针体后再删不净会留坏方法；
    // 宁整根删除，引用普查关死）
    for (ca::usize mi = 0; mi < cf.methods.size(); ++mi) {
        jbc::core::MemberInfo& m = cf.methods[mi];
        if (junkMethodName(cf.utf8(m.nameCp))) continue;
        ca::i64                codeSlot = -1;
        for (ca::usize ai = 0; ai < m.attrs.size(); ++ai) {
            if (cf.utf8(m.attrs[ai].nameCp) == "Code") {
                codeSlot = static_cast<ca::i64>(ai);
                break;
            }
        }
        if (codeSlot < 0) continue;
        auto d = cf.decodeAttr(m.attrs[static_cast<ca::usize>(codeSlot)]);
        if (d.is_err()) {
            ++stats.methodsFailClosed;
            continue;
        }
        if (!ca::core::tag_cast::isa<jbc::core::CodeAttr>(d.unwrap())) {
            ++stats.methodsFailClosed;
            continue;
        }
        auto* bodyPtr
            = ca::core::tag_cast::cast<jbc::core::CodeAttr>(d.unwrap());
        if (bodyPtr == nullptr) {
            ++stats.methodsFailClosed;
            continue;
        }
        JunkRegion region;
        if (detectRegion(cf, bodyPtr->code, *bodyPtr, region, mi, stats)) {
            if (!region.spans.empty()) {
                if (trace) {
                    std::string spanText;
                    for (const auto& [s, e] : region.spans) {
                        spanText += " [" + std::to_string(s) + "," + std::to_string(e) + ")";
                    }
                    std::fprintf(stderr,
                                 "[grunt.JunkMemberStrip] region %s.%s spans=%s\n",
                                 cand.owner.c_str(), std::string(cf.utf8(m.nameCp)).c_str(),
                                 spanText.c_str());
                }
                cand.regions.push_back(std::move(region));
            }
        } else if (trace) {
            std::fprintf(stderr, "[grunt.JunkMemberStrip] abandon %s.%s %s\n",
                         cand.owner.c_str(), std::string(cf.utf8(m.nameCp)).c_str(),
                         g_lastAbandon.c_str());
        }
    }
    return cand;
}

/// @brief CP 快照：Fieldref/Methodref 全集 + 字符串常量（反射按名守卫）。
struct CpSnap
{
    std::set<std::string> methodKeys;   // owner::name::desc
    std::set<std::string> fieldKeys;    // owner::name
    std::set<std::string> names;        // 字符串常量内容
};

CpSnap collectCpSnap(const jbc::core::ClassFile& cf)
{
    CpSnap out;
    using T = jbc::core::CpTag;
    const jbc::core::ConstantPool& cp = cf.cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (!cp.valid(i)) continue;
        const auto& e = cp.at(i);
        if (e.tag == T::String) {
            if (cp.valid(e.a)) out.names.insert(std::string(cf.utf8(e.a)));
            continue;
        }
        if (e.tag != T::Fieldref && e.tag != T::Methodref
            && e.tag != T::InterfaceMethodref) {
            continue;
        }
        if (!cp.valid(e.a) || !cp.valid(e.b)) continue;
        const auto& cls = cp.at(e.a);
        const auto& nat = cp.at(e.b);
        if (cls.tag != T::Class || nat.tag != T::NameAndType) continue;
        if (!cp.valid(cls.a) || !cp.valid(nat.a) || !cp.valid(nat.b)) continue;
        const std::string owner(cf.utf8(cls.a));
        const std::string name(cf.utf8(nat.a));
        const std::string desc(cf.utf8(nat.b));
        if (e.tag == T::Fieldref) {
            out.fieldKeys.insert(owner + "::" + name);
        } else {
            out.methodKeys.insert(owner + "::" + name + "::" + desc);
        }
    }
    return out;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;
    static const bool trace = std::getenv("GRUNT_JMS_TRACE") != nullptr;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        try {
            ClassCand cand = scanClass(*session, std::string(entry.uri), result);
            const bool hasWork = !cand.regions.empty() || !cand.methodIdxs.empty()
                                 || !cand.fieldIdxs.empty();
            if (trace) {
                std::fprintf(stderr,
                             "[grunt.JunkMemberStrip] scan %s regions=%zu methods=%zu"
                             " fields=%zu\n",
                             cand.owner.c_str(), cand.regions.size(),
                             cand.methodIdxs.size(), cand.fieldIdxs.size());
            }
            if (hasWork) result.classes.push_back(std::move(cand));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[grunt.JunkMemberStrip] scan 类跳过：%s（%s）\n",
                         std::string(entry.uri).c_str(), e.what());
            ++result.classesSkipped;
        } catch (...) {
            std::fprintf(stderr, "[grunt.JunkMemberStrip] scan 类跳过：%s（SEH）\n",
                         std::string(entry.uri).c_str());
            ++result.classesSkipped;
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// @brief SMT 帧摘除 + delta 重排（same/same1 帧超界升格 extended 形）。
/// @return false = 属性解不开（原样保真，宁留勿猜）。
bool dropFramesAt(ir::ClassSession& s, jbc::core::CodeAttr& body,
                  const std::vector<ca::u32>& dropPcs)
{
    if (dropPcs.empty()) return true;
    using jbc::core::StackMapTableAttr;
    jbc::core::ClassFile& cf = s.backing();
    for (jbc::core::AttrSlot& slot : body.attrs) {
        if (cf.utf8(slot.nameCp) != "StackMapTable") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        if (!ca::core::tag_cast::isa<StackMapTableAttr>(d.unwrap())) return false;
        auto* smt = ca::core::tag_cast::cast<StackMapTableAttr>(d.unwrap());
        if (smt == nullptr) return false;
        // 绝对 pc 重算 + 摘除锚在 dropPcs 的帧
        std::vector<StackMapTableAttr::Frame> kept;
        std::vector<ca::u32>                  keptPcs;
        ca::i64                               prev = -1;
        for (StackMapTableAttr::Frame& f : smt->frames) {
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
                return false;
            }
            const ca::u32 pc = (prev < 0)
                                   ? delta
                                   : static_cast<ca::u32>(prev) + delta + 1;
            prev = pc;
            const bool drop = std::find(dropPcs.begin(), dropPcs.end(), pc)
                              != dropPcs.end();
            if (!drop) {
                kept.push_back(std::move(f));
                keptPcs.push_back(pc);
            }
        }
        // delta 重排（JVMS 4.7.4 链式算术）；same/same1 帧超界升格 extended
        for (ca::usize k = 0; k < kept.size(); ++k) {
            const ca::u32 prevPc = (k == 0) ? 0 : keptPcs[k - 1];
            const ca::u32 target = keptPcs[k];
            const ca::u32 delta = (k == 0) ? target : target - prevPc - 1;
            StackMapTableAttr::Frame& f = kept[k];
            if (f.type <= StackMapTableAttr::Frame::kSameMax) {
                if (delta <= StackMapTableAttr::Frame::kSameMax) {
                    f.type = static_cast<ca::u8>(delta);
                } else {                          // 升格 same_frame_extended
                    f.type   = StackMapTableAttr::Frame::kSameExt;
                    f.offset = delta;
                }
            } else if (f.type >= StackMapTableAttr::Frame::kSame1Base
                       && f.type <= StackMapTableAttr::Frame::kSame1Max) {
                if (delta <= StackMapTableAttr::Frame::kSame1Max
                              - StackMapTableAttr::Frame::kSame1Base) {
                    f.type = static_cast<ca::u8>(
                        StackMapTableAttr::Frame::kSame1Base + delta);
                } else {                          // 升格 same_locals_1_stack_item_extended
                    f.type   = StackMapTableAttr::Frame::kSame1Ext;
                    f.offset = delta;
                }
            } else {
                f.offset = delta;                 // chop/same_ext/append/full：offset 即 delta
            }
        }
        auto* mut = ca::core::tag_cast::cast<StackMapTableAttr>(slot.mutate());
        if (mut == nullptr) return false;
        mut->frames = std::move(kept);
    }
    return true;
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    static const bool trace = std::getenv("GRUNT_JMS_TRACE") != nullptr;

    // (1) 逐类 NOP 化（scan 定死的段；长度保持；SMT 帧摘除）
    {
        std::unordered_map<std::string, const ClassCand*> byUri;
        for (const ClassCand& cand : result.classes) byUri[cand.uri] = &cand;
        std::vector<std::pair<std::string, std::vector<ca::u8>>> pending;
        forEachClass(container, [&](const ClassEntry& entry) {
            const auto it = byUri.find(std::string(entry.uri));
            if (it == byUri.end() || it->second->regions.empty()) return;
            auto session = openSession(*entry.bytes);
            if (!session) return;
            jbc::core::ClassFile& cf = session->backing();
            const ClassCand&      cand = *it->second;
            bool changed = false;
            for (const JunkRegion& region : cand.regions) {
                if (region.methodIndex >= cf.methods.size()) continue;
                jbc::core::MemberInfo& m = cf.methods[region.methodIndex];
                ca::i64                codeSlot = -1;
                for (ca::usize ai = 0; ai < m.attrs.size(); ++ai) {
                    if (cf.utf8(m.attrs[ai].nameCp) == "Code") {
                        codeSlot = static_cast<ca::i64>(ai);
                        break;
                    }
                }
                if (codeSlot < 0) continue;
                auto d = cf.decodeAttr(m.attrs[static_cast<ca::usize>(codeSlot)]);
                if (d.is_err()) continue;
                auto* mutBody = ca::core::tag_cast::cast<jbc::core::CodeAttr>(
                    m.attrs[static_cast<ca::usize>(codeSlot)].mutate());
                if (mutBody == nullptr) continue;
                ca::usize bytes = 0;
                for (const auto& [start, end] : region.spans) {
                    if (end > mutBody->code.size()) continue;
                    for (ca::u32 p = start; p < end; ++p) mutBody->code[p] = kNop;
                    bytes += end - start;
                }
                if (bytes == 0) continue;
                if (!dropFramesAt(*session, *mutBody, region.droppedFramePcs)) continue;
                dirty.nopBytes += bytes;
                ++dirty.regions;
                dirty.athrows += region.athrowCount;
                changed = true;
            }
            if (!changed) return;
            auto encoded = jbc::core::writeClass(cf);
            if (encoded.is_err()) return;
            pending.emplace_back(std::string(entry.uri), std::move(encoded).unwrap());
        });
        for (auto& [uri, bytes] : pending) {
            replaceClass(container, uri, std::move(bytes));
            dirty.uris.push_back(uri);
        }
        if (trace) {
            std::fprintf(stderr, "[grunt.JunkMemberStrip] nop 类=%zu 段=%zu 字节=%zu\n",
                         dirty.uris.size(), dirty.regions, dirty.nopBytes);
        }
    }

    // (2) 候选类 CP 压缩（注解深度预检；失败类原样保真，引用照入普查）。
    //     只压候选类：非候选类的池本来就无 junk 引用可带走，压缩是纯搅动
    //     （干净类字节零变化是验收面）；其池条目照原样入普查，宁留方向。
    std::set<std::string> compactFailed;
    {
        std::set<std::string> candUris;
        for (const ClassCand& cand : result.classes) candUris.insert(cand.uri);
        std::vector<std::pair<std::string, std::vector<ca::u8>>> snapshot;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (candUris.count(std::string(entry.uri)) == 0) return;
            snapshot.emplace_back(std::string(entry.uri), *entry.bytes);
        });
        for (auto& [uri, bytes] : snapshot) {
            try {
                auto session = openSession(bytes);
                if (!session) {
                    compactFailed.insert(uri);
                    continue;
                }
                if (!classAnnoDepthSafe(session->backing())) {
                    compactFailed.insert(uri);
                    continue;
                }
                if (ir::compactConstantPool(*session).is_err()) {
                    compactFailed.insert(uri);
                    continue;
                }
                auto encoded = jbc::core::writeClass(session->backing());
                if (encoded.is_err()) {
                    compactFailed.insert(uri);
                    continue;
                }
                std::vector<ca::u8> fresh = std::move(encoded).unwrap();
                if (fresh != bytes) {
                    replaceClass(container, uri, std::move(fresh));
                    ++dirty.compacted;
                    bool counted = false;
                    for (const std::string& u : dirty.uris) counted |= (u == uri);
                    if (!counted) dirty.uris.push_back(uri);
                }
            } catch (...) {
                compactFailed.insert(uri);
            }
        }
    }

    // (3) 活引用普查（分层兜底；census-blind 整轮放弃）
    std::set<std::string> liveMethods;
    std::set<std::string> liveFields;
    std::set<std::string> liveNames;
    {
        ca::usize rawSources = 0;
        ca::usize blind      = 0;
        pass_clock::Suspend suspend;
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (session) {
                try {
                    CpSnap snap = collectCpSnap(session->backing());
                    liveMethods.insert(snap.methodKeys.begin(), snap.methodKeys.end());
                    liveFields.insert(snap.fieldKeys.begin(), snap.fieldKeys.end());
                    liveNames.insert(snap.names.begin(), snap.names.end());
                    return;
                } catch (...) {
                    std::fprintf(stderr,
                                 "[grunt.JunkMemberStrip] 普查走原始字节路径：%s\n",
                                 std::string(entry.uri).c_str());
                }
            }
            ++rawSources;
            auto raw = jbc::core::readClass(jbc::core::ByteView(*entry.bytes),
                                            jbc::core::ReadOptions{});
            if (raw.is_err()) {
                ++blind;
                std::fprintf(stderr,
                             "[grunt.JunkMemberStrip] 普查盲区：%s（引用证据不可得）\n",
                             std::string(entry.uri).c_str());
                return;
            }
            jbc::core::ReadResult parsed = std::move(raw).unwrap();
            if (parsed.file == nullptr) {
                ++blind;
                std::fprintf(stderr,
                             "[grunt.JunkMemberStrip] 普查盲区：%s（骨架模型缺失）\n",
                             std::string(entry.uri).c_str());
                return;
            }
            CpSnap snap = collectCpSnap(*parsed.file);
            liveMethods.insert(snap.methodKeys.begin(), snap.methodKeys.end());
            liveFields.insert(snap.fieldKeys.begin(), snap.fieldKeys.end());
            liveNames.insert(snap.names.begin(), snap.names.end());
        });
        dirty.censusRawSources = rawSources;
        dirty.censusBlind      = blind;
        if (blind != 0) {
            std::fprintf(stderr,
                         "[grunt.JunkMemberStrip] census-blind %zu 类：成员删除整轮放弃\n",
                         blind);
            countResidual(container, dirty.residualMethods, dirty.residualFields);
            return dirty;
        }
    }

    // (4) 逐类删 junk 成员（引用还活着 → 宁留；backing 序倒序 erase）
    {
        std::unordered_map<std::string, const ClassCand*> byUri;
        for (const ClassCand& cand : result.classes) byUri[cand.uri] = &cand;
        std::vector<std::pair<std::string, std::vector<ca::u8>>> pending;
        forEachClass(container, [&](const ClassEntry& entry) {
            const std::string uri(entry.uri);
            if (compactFailed.count(uri) != 0) return;
            const auto it = byUri.find(uri);
            if (it == byUri.end()) return;
            const ClassCand& cand = *it->second;
            if (cand.methodIdxs.empty() && cand.fieldIdxs.empty()) return;
            try {
                auto session = openSession(*entry.bytes);
                if (!session) return;
                jbc::core::ClassFile& cf = session->backing();
                const std::string     owner(cand.owner);
                std::set<ca::usize>   dropMethods;
                std::set<ca::usize>   dropFields;
                for (const ca::usize mi : cand.methodIdxs) {
                    if (mi >= cf.methods.size()) continue;
                    ++dirty.attempted;
                    const std::string name(cf.utf8(cf.methods[mi].nameCp));
                    const std::string desc(cf.utf8(cf.methods[mi].descCp));
                    const std::string key = owner + "::" + name + "::" + desc;
                    if (liveNames.count(name) != 0 || liveMethods.count(key) != 0) {
                        ++dirty.rejectedByRef;
                        continue;
                    }
                    dropMethods.insert(mi);
                }
                for (const ca::usize fi : cand.fieldIdxs) {
                    if (fi >= cf.fields.size()) continue;
                    ++dirty.attempted;
                    const std::string name(cf.utf8(cf.fields[fi].nameCp));
                    const std::string key = owner + "::" + name;
                    if (liveNames.count(name) != 0 || liveFields.count(key) != 0) {
                        ++dirty.rejectedByRef;
                        continue;
                    }
                    dropFields.insert(fi);
                }
                if (dropMethods.empty() && dropFields.empty()) return;
                for (auto rit = dropMethods.rbegin(); rit != dropMethods.rend(); ++rit) {
                    cf.methods.erase(cf.methods.begin()
                                     + static_cast<ca::i64>(*rit));
                    ++dirty.methods;
                }
                for (auto rit = dropFields.rbegin(); rit != dropFields.rend(); ++rit) {
                    cf.fields.erase(cf.fields.begin() + static_cast<ca::i64>(*rit));
                    ++dirty.fields;
                }
                auto encoded = jbc::core::writeClass(cf);
                if (encoded.is_err()) return;
                pending.emplace_back(uri, std::move(encoded).unwrap());
            } catch (...) {
                std::fprintf(stderr, "[grunt.JunkMemberStrip] 删成员类跳过：%s\n",
                             uri.c_str());
            }
        });
        for (auto& [uri, bytes] : pending) {
            replaceClass(container, uri, std::move(bytes));
            bool counted = false;
            for (const std::string& u : dirty.uris) counted |= (u == uri);
            if (!counted) dirty.uris.push_back(uri);
        }
    }

    // (5) 候选类删除后再压缩一次，把描述符 utf8 带走（非候选类不搅动）
    {
        std::set<std::string> candUris;
        for (const ClassCand& cand : result.classes) candUris.insert(cand.uri);
        std::vector<std::pair<std::string, std::vector<ca::u8>>> after;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (candUris.count(std::string(entry.uri)) == 0) return;
            after.emplace_back(std::string(entry.uri), *entry.bytes);
        });
        for (auto& [uri, bytes] : after) {
            try {
                auto session = openSession(bytes);
                if (!session) continue;
                if (!classAnnoDepthSafe(session->backing())) continue;
                if (ir::compactConstantPool(*session).is_err()) continue;
                auto encoded = jbc::core::writeClass(session->backing());
                if (encoded.is_err()) continue;
                std::vector<ca::u8> fresh = std::move(encoded).unwrap();
                if (fresh != bytes) {
                    replaceClass(container, uri, std::move(fresh));
                    bool counted = false;
                    for (const std::string& u : dirty.uris) counted |= (u == uri);
                    if (!counted) {
                        dirty.uris.push_back(uri);
                        ++dirty.compacted;
                    }
                }
            } catch (...) {
                // 二次压缩失败的类保有删成员后的字节，不再压缩
            }
        }
    }

    countResidual(container, dirty.residualMethods, dirty.residualFields);
    if (trace) {
        std::fprintf(stderr,
                     "[grunt.JunkMemberStrip] rewrite 段=%zu 字节=%zu 方法=%zu 字段=%zu"
                     " 拒=%zu residual(m/f)=%zu/%zu\n",
                     dirty.regions, dirty.nopBytes, dirty.methods, dirty.fields,
                     dirty.rejectedByRef, dirty.residualMethods, dirty.residualFields);
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
    countResidual(container, result.residualMethods, result.residualFields);
    if (result.residualMethods != dirty.residualMethods
        || result.residualFields != dirty.residualFields) {
        // 容器在 rewrite 后又被改过才会出现；按失配记账（本 pass 内不该发生）
        result.ok = false;
        result.failed.emplace_back("residual-count");
    }
    return result;
}

}   // namespace grunt::junk_member_strip
