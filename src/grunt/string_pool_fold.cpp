/// @file        string_pool_fold.cpp
/// @brief       残留 `string_pool_fold` 三段实现：池链识别（clinit 前导 +
///              arrayInit 块序列 + 解密器三窗口，S2）→ 调用点三元组折回
///              `ldc` 明文（S1）→ residual + 类数 + 重解析验收。
///
/// 形态、判据分级与锚保留/槽位面设计见头文件；骨架对齐 string_cache_fold
/// （读点折叠）：scan 只读、rewrite 只写（mutateMethodWithFrames 正统帧链
/// 写回）、proof 只判（类数不增 + 脏类可重解析 + residual 统计）。解密算
/// 法是家族知识转写（grunt2@36c0eef / grunt3@e41e852 逐行同源，源锚点见各
/// 函数头注与 knowledge/grunt_names.hpp）。
///
/// ## SCAN
///
/// 池链三件套钉形（buildPoolChain）：clinit 内全量滑窗扫<int poolLen>;
/// ANEWARRAY java/lang/String; PUTSTATIC <self String[] static>;
/// INVOKESTATIC <self> ()V前导四连，多个不同解 fail-closed（多个池分配
/// 前导）；池字段必须在场且 static。前导引用的 ()V 允许是直线静态蹦床（体
/// 恰 `INVOKESTATIC <self> ()V; RETURN`，深度封 2、目标异于自身防环），
/// Grunteon 全链把 arrayInit 调用藏进 indy 机器，上游 IndyDecode/IndyFold
/// 还原后即此形；`[INVOKEDYNAMIC; RETURN]` 形态报独立拒绝原因（arrayInit
/// 调用被 indy 蹦床隐藏），不评 BSM。parseArrayInit 钉整段直线（无标签/无
/// 异常表/无分支）的逐块 store 序列，块数与前导池长核对；形态判别按常量
/// KIND：密文位是 String 常量 → literal（钉 toCharArray 形），int 常量 →
/// arrayed（len 经 iconst/bipush/ldc 任意装载，NEWARRAY T_CHAR 钉 a==5，
/// 码元组 `DUP; <int e>; <int code>; CASTORE` 下标乱序即拒）。解密器三窗
/// 口（核窗口 52 条逐字 + init 窗口 + 循环头窗口）：核窗口唯一命中才放
/// 行，槽位 state/counter 自捕获核对；旋转掩码钉常量值 255 而不是 opcode
/// ，汇编码归一后 `sipush 255` 可能成 `ldc 255`，钉 opcode 会失配。
///
/// 读取点三元组：全类方法扫 `GETSTATIC <池>; <int idx>; AALOAD`。跨度内
/// NOP 跳过（squeeze 遗留落点不驻栈，number_fold 同口径）；含标签（合并
/// 点）即 fail-closed；下标非常量 = 非本家三元组静默跳过（idxMissing）；
/// 池链失败的类整类计数，无前导的 String[] 字段类（Kasumi 缓存同形）静默
/// 跳过，家族检查由池链结构自证：Kasumi 同形三元组不带 Grunt 池分配前导与
/// `([CJI)` 填池链，结构上不可命中。
///
/// ## REWRITE
///
/// 按 uri 聚类 + roundTripSafe 前置检查。锚点（getstatic）原地换 ldc 明文
/// （CodeEditor::loadConst 按索引选 LDC/LDC_W，srcPc 保留，pcmap/调试行落
/// 点不变）。明文落池先 findUtf8 / 线性扫 String 复用，miss 才 addUtf8 /
/// addString（先 intern 后动 DOM）。池字段 / arrayInit / 解密器不在此删：
/// 供给链仍被 clinit 前导引用而存活（运行期空转填池，无读方），整体摘除
/// 属压池面的后续工作。失败粒度 = 类：帧链/编码失败整类放弃。
///
/// ## PROOF
///
/// pass_clock::Suspend + 类数不增 + 脏类可重新解析 + residual：改写后容器
/// 里池链仍成立且读取点仍为三元组的站点数（scan 同判据 countOnly 重算）。
/// 语料单 jar 检查要求 residual = 0。
///
/// ## 已知边界
///
/// Grunteon 全链 jar 上全量池类的 arrayInit 调用被 indy 蹦床隐藏，本 pass 单
/// 跑整体 fail-closed 是正确行为不是缺陷：判据要求供给链静态在场，调用目
/// 标藏在加密 BSM 里时证据链不完整，不猜；上游 IndyRestore/IndyDecode 还
/// 原蹦床后直线跟随即可接通（单测 FoldsThroughStaticTrampoline 验证该形可
/// 折）。经局部变量/方法调用间接取下标的形态不在面内（Grunt 生成面不产此
/// 形）。interface 池类（PUBLIC+FINAL 字段变体）未钉 access 差异，desc/
/// owner/static 已足够定案，access 面留待有样本时再收窄。

#include "grunt/string_pool_fold.hpp"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/pass_clock.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::string_pool_fold {

namespace {

namespace ir = jbc::ir;
using jbc::core::CpTag;
using jbc::core::Op;

constexpr ca::u32   kAccStatic = 0x0008;
constexpr ca::usize kNoSlot    = static_cast<ca::usize>(-1);

// ============================================================================
// ==== UTF-16 码元 ↔ classfile modified UTF-8（明文 → 常量池 Utf8 字节面）===
// ============================================================================

/// @brief UTF-16 码元序列 → classfile modified UTF-8（NUL 走 0xC0 0x80；
///        代理对按码元各落 3 字节形，javac 写常量池的同构口径）。全码元
///        可表示，无失败面。
void u16ToMutf8(const std::vector<ca::u16>& units, std::string& out)
{
    out.clear();
    for (const ca::u16 u : units) {
        if (u == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
}

/// @brief UTF-16 码元序列 → UTF-8 记录形（有效代理对重组 4 字节；孤立代理
///        返回 false）。记录形进 Site.plaintext（trace/统计/单测），孤立
///        代理不可表示即整站拒绝（javac ldc 源语言面不含孤立代理，语料
///        不受影响；fail-closed 不猜）。
bool u16ToUtf8Record(const std::vector<ca::u16>& units, std::string& out)
{
    out.clear();
    for (ca::usize i = 0; i < units.size(); ++i) {
        const ca::u16 u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= units.size()) return false;
            const ca::u16 lo = units[i + 1];
            if (lo < 0xDC00 || lo > 0xDFFF) return false;
            const ca::u32 cp = 0x10000u
                             + ((static_cast<ca::u32>(u - 0xD800) << 10)
                                | static_cast<ca::u32>(lo - 0xDC00));
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            ++i;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return false;   // 孤立低位代理
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return true;
}

/// @brief 常量池 Utf8（modified UTF-8）→ UTF-16 码元序列。3 字节代理形按
///        码元直取不重组，literal 密文串的 `toCharArray` 就是码元语义。
///        字节形非法（挂续/过短编码）返回 false，整块拒绝。
bool mutf8ToU16(std::string_view bytes, std::vector<ca::u16>& out)
{
    out.clear();
    ca::usize i = 0;
    while (i < bytes.size()) {
        const ca::u8 b0 = static_cast<ca::u8>(bytes[i]);
        if (b0 < 0x80) {
            if (b0 == 0) return false;   // MUTF-8 的 NUL 走 C0 80，裸 0x00 非法
            out.push_back(static_cast<ca::u16>(b0));
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            if (bytes.size() - i < 2) return false;
            const ca::u8 b1 = static_cast<ca::u8>(bytes[i + 1]);
            if ((b1 & 0xC0) != 0x80) return false;
            const ca::u16 u =
                static_cast<ca::u16>((static_cast<ca::u16>(b0 & 0x1F) << 6) | (b1 & 0x3F));
            if (u < 0x80) return false;   // 过短编码
            out.push_back(u);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            if (bytes.size() - i < 3) return false;
            const ca::u8 b1 = static_cast<ca::u8>(bytes[i + 1]);
            const ca::u8 b2 = static_cast<ca::u8>(bytes[i + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            const ca::u16 u = static_cast<ca::u16>(
                (static_cast<ca::u16>(b0 & 0x0F) << 12) | (static_cast<ca::u16>(b1 & 0x3F) << 6)
                | (b2 & 0x3F));
            if (u < 0x800) return false;   // 过短编码（含 C0 80 的 3 字节冒名形）
            out.push_back(u);
            i += 3;
        } else {
            return false;   // MUTF-8 无 4 字节形
        }
    }
    return true;
}

// ============================================================================
// ==== 解密算法转写（家族知识；源锚点 grunt2@36c0eef / grunt3@e41e852）=======
// ============================================================================

/// @brief Grunt 字符串 XOR 流。源：grunt2 分支 36c0eef
///        `StringEncryptTransformer.kt` 的 `encrypt(cArray, seed, key,
///        classKey)` 逐行转写；grunt3 分支 e41e852
///        `StringArrayedEncrypt.kt` 的 `encrypt` 逐字相同。解密器体
///        （createDecryptMethod）是同一算术核，
///
///            var n = key; var l = seed; var n2 = classKey xor n
///            for (i in cArray.indices) {
///                n2 = n2 xor l.toInt() xor i.inv()
///                n2 = n2 xor (n - i * cArray.size)
///                n2 = (-n2 * n) or i
///                cArray[i] = (cArray[i].code xor n2).toChar()
///                val n3 = i and 0xFF
///                n = (n shl n3) or (n ushr -n3)
///                l = l xor n3.toLong()
///            }
///
///        自逆性：状态演化只依赖 (classKey, key, seed, len)，与密文无关，
///        同一函数作用在密文上即得明文（XOR 流对称）。JVM int 语义（溢出
///        回绕、移位计数 & 0x1F）在无符号域显式完成，杜绝 C++ UB。
void gruntCharXorStream(std::vector<ca::u16>& chars, ca::i64 seed, ca::i32 key, ca::i32 classKey)
{
    ca::u32 n  = static_cast<ca::u32>(key);
    ca::i64 l  = seed;
    ca::u32 n2 = static_cast<ca::u32>(classKey) ^ n;
    const ca::u32 len = static_cast<ca::u32>(chars.size());

    for (ca::u32 i = 0; i < len; ++i) {
        n2 ^= static_cast<ca::u32>(static_cast<ca::i32>(l)) ^ (~i);
        n2 ^= static_cast<ca::u32>(static_cast<ca::i32>(n)) - (i * len);
        n2  = static_cast<ca::u32>(static_cast<ca::i32>(0u - n2) * static_cast<ca::i32>(n)) | i;
        chars[i] = static_cast<ca::u16>(chars[i] ^ n2);
        const ca::u32 n3 = i & 0xFFu;
        n = (n << (n3 & 31u)) | (n >> ((0u - n3) & 31u));
        l ^= static_cast<ca::i64>(n3);
    }
}

// ============================================================================
// ==== 形状钉面辅助（S2；常量装载读数走 const_args）==========================
// ============================================================================

/// @brief int 槽装载（ILOAD a==slot 或 ILOAD_0..3 快捷形）。
bool intLoadAt(const ir::Insn& in, ca::u32 slot)
{
    if (in.op == Op::ILOAD) return in.a == static_cast<ca::i32>(slot);
    if (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3) {
        return slot <= 3
            && static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ILOAD_0)) == slot;
    }
    return false;
}

/// @brief int 槽存储（ISTORE a==slot 或 ISTORE_0..3 快捷形）。
bool intStoreAt(const ir::Insn& in, ca::u32 slot)
{
    if (in.op == Op::ISTORE) return in.a == static_cast<ca::i32>(slot);
    if (in.op >= Op::ISTORE_0 && in.op <= Op::ISTORE_3) {
        return slot <= 3
            && static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ISTORE_0)) == slot;
    }
    return false;
}

/// @brief long 槽装载（LLOAD a==slot 或 LLOAD_0..3 快捷形）。
bool longLoadAt(const ir::Insn& in, ca::u32 slot)
{
    if (in.op == Op::LLOAD) return in.a == static_cast<ca::i32>(slot);
    if (in.op >= Op::LLOAD_0 && in.op <= Op::LLOAD_3) {
        return slot <= 3
            && static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::LLOAD_0)) == slot;
    }
    return false;
}

/// @brief long 槽存储（LSTORE a==slot 或 LSTORE_0..3 快捷形）。
bool longStoreAt(const ir::Insn& in, ca::u32 slot)
{
    if (in.op == Op::LSTORE) return in.a == static_cast<ca::i32>(slot);
    if (in.op >= Op::LSTORE_0 && in.op <= Op::LSTORE_3) {
        return slot <= 3
            && static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::LSTORE_0)) == slot;
    }
    return false;
}

/// @brief int 槽装载读取（ILOAD a / ILOAD_0..3 → 槽号进 slot；失败 false）。
bool intLoadSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ILOAD) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ILOAD_0));
        return true;
    }
    return false;
}

/// @brief int 槽存储读取（ISTORE a / ISTORE_0..3 → 槽号进 slot；失败 false）。
bool intStoreSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ISTORE) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ISTORE_0 && in.op <= Op::ISTORE_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ISTORE_0));
        return true;
    }
    return false;
}

/// @brief long 槽装载读取（LLOAD a / LLOAD_0..3 → 槽号进 slot；失败 false）。
bool longLoadSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::LLOAD) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::LLOAD_0 && in.op <= Op::LLOAD_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::LLOAD_0));
        return true;
    }
    return false;
}

/// @brief char[] 参数装载（ALOAD 0 / ALOAD_0，static 方法首参槽恒 0）。
bool arrayLoadZeroAt(const ir::Insn& in)
{
    if (in.op == Op::ALOAD) return in.a == 0;
    return in.op == Op::ALOAD_0;
}

/// @brief 从 from 起跳过 NOP 找下一条真实指令（squeeze 遗留落点不驻栈，
///        number_fold 同口径）；途中任何标签置 labeledFlag（合并点计数）。
const ir::Insn* nextRealInsn(ir::CodeDom& dom, const std::vector<ir::InsnId>& order,
                             ca::usize from, ca::usize* outQ, bool* labeledFlag)
{
    for (ca::usize q = from; q < order.size(); ++q) {
        const ir::Insn* in = dom.peek(order[q]);
        if (in == nullptr) return nullptr;
        if (!dom.labelsAt(order[q]).empty()) *labeledFlag = true;
        if (in->op != Op::NOP) {
            *outQ = q;
            return in;
        }
    }
    return nullptr;
}

/// @brief 方法骨架定位：按名字 + 描述符在 unit.methods 找唯一槽。
/// @return 方法槽；kNoSlot = 不存在。
ca::usize findMethodSlot(const ir::ClassUnit& unit, std::string_view name,
                         std::string_view desc)
{
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        const ir::MethodUnit& mu = unit.methods[mi];
        if (unit.symbols().str(mu.name) == name && unit.symbols().str(mu.desc) == desc) {
            return mi;
        }
    }
    return kNoSlot;
}

// ============================================================================
// ==== 池链识别（S2：前导 + arrayInit 块序列 + 解密器三窗口）=================
// ============================================================================

/// @brief 池表条目：密文 + 解密参数（arrayInit store 块逐字提取）。
struct PoolEntry
{
    std::vector<ca::u16> cipher;
    ca::i64              seed = 0;
    ca::i32              key  = 0;
    CipherForm           form = CipherForm::Literal;
    std::vector<ca::u16> plain;   ///< 解密产物码元（gruntCharXorStream(cipher)）
};

/// @brief 一条已验证的池链（Grunt 生成面三件套 + 解密参数全在场）。
struct PoolChain
{
    std::string                            fieldName;
    ca::i32                                poolLen = 0;
    std::unordered_map<ca::i32, PoolEntry> entries;
    ca::i32                                classKey = 0;
};

/// @brief 池链失败桶（计数归类；None = 成功）。
enum class ChainFail : ca::u8 { None, NoPrelude, ArrayInit, Decryptor };

/// @brief 一次 promote 的会话级产物（arrayInit / 解密器检查共用）。
struct PromotedCode
{
    std::vector<const ir::Insn*> seq;   ///< dom 序的非空指令视图
    ir::CodeUnit*                cu = nullptr;
};

/// @brief promote 单方法并把指令序摊平成非空指令视图（失败返回 false）。
bool promoteToSeq(ir::ClassSession& s, ca::usize methodIndex, PromotedCode& out)
{
    if (!ir::hasCode(s, methodIndex)) return false;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return false;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;
    out.cu = cu;
    out.seq.clear();
    out.seq.reserve(cu->dom.order().size());
    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) return false;
        out.seq.push_back(in);
    }
    return true;
}

/// @brief 解密器三窗口钉形：init（classKey 烧体）+ 循环头（bound=len）+ 核
///        （52 条算术核心逐字）。全部命中且槽位核对一致、解法唯一才放行。
///        后续加密面变换（ArithmeticEncrypt/Controlflow）打碎任一窗口 →
///        整类拒绝（fail-closed，residual 如实计）。
/// @return 是否提取成功（classKey 进 out；失败原因进 why）。
bool extractClassKey(ir::ClassSession& s, const PromotedCode& code, std::string& why,
                     ca::i32& classKey)
{
    const std::vector<const ir::Insn*>& seq = code.seq;

    // ---- 核窗口（52 条）：从 iload <state> 到 lstore <seed>，逐字钉形 ----
    // 槽位：state(n2)/counter(i)/key(k)/seed(l)/tmp(t)；k、l 由 desc ([CJI)
    // 的静态槽位唯一决定（key=3、seed=1）。源序逐条对应 createDecryptMethod
    // 的 L4~L11 块（grunt2@36c0eef / grunt3@e41e852，语料 boar-main 同形）。
    using CoreSlots = std::tuple<ca::u32, ca::u32, ca::u32, ca::u32, ca::u32>;   // s,i,k,l,t
    std::vector<CoreSlots> cores;
    if (seq.size() >= 52) {
        for (ca::usize p = 0; p + 52 <= seq.size(); ++p) {
            ca::u32 st = 0, i = 0, k = 0, l = 0, t = 0;
            if (!intLoadSlotOf(*seq[p], st)) continue;
            if (!longLoadSlotOf(*seq[p + 1], l) || !longStoreAt(*seq[p + 51], l)) continue;
            if (seq[p + 2]->op != Op::L2I || seq[p + 3]->op != Op::IXOR) continue;
            if (!intLoadSlotOf(*seq[p + 4], i)) continue;
            if (seq[p + 5]->op != Op::ICONST_M1 || seq[p + 6]->op != Op::IXOR
                || seq[p + 7]->op != Op::IXOR || !intStoreAt(*seq[p + 8], st)) continue;
            if (!intLoadAt(*seq[p + 9], st) || !intLoadSlotOf(*seq[p + 10], k)
                || !intLoadAt(*seq[p + 11], i) || !arrayLoadZeroAt(*seq[p + 12])
                || seq[p + 13]->op != Op::ARRAYLENGTH || seq[p + 14]->op != Op::IMUL
                || seq[p + 15]->op != Op::ISUB || seq[p + 16]->op != Op::IXOR
                || !intStoreAt(*seq[p + 17], st)) continue;
            if (!intLoadAt(*seq[p + 18], st) || seq[p + 19]->op != Op::INEG
                || !intLoadAt(*seq[p + 20], k) || seq[p + 21]->op != Op::IMUL
                || !intLoadAt(*seq[p + 22], i) || seq[p + 23]->op != Op::IOR
                || !intStoreAt(*seq[p + 24], st)) continue;
            if (!arrayLoadZeroAt(*seq[p + 25]) || !intLoadAt(*seq[p + 26], i)
                || !arrayLoadZeroAt(*seq[p + 27]) || !intLoadAt(*seq[p + 28], i)
                || seq[p + 29]->op != Op::CALOAD || !intLoadAt(*seq[p + 30], st)
                || seq[p + 31]->op != Op::IXOR || seq[p + 32]->op != Op::I2C
                || seq[p + 33]->op != Op::CASTORE) continue;
            ca::i32 mask = 0;
            if (!intLoadAt(*seq[p + 34], i) || !const_args::decodeConstInt(s, *seq[p + 35], mask)
                || mask != knowledge::kGruntRotateMask
                || seq[p + 36]->op != Op::IAND || !intStoreSlotOf(*seq[p + 37], t)) continue;
            if (!intLoadAt(*seq[p + 38], k) || !intLoadAt(*seq[p + 39], t)
                || seq[p + 40]->op != Op::ISHL || !intLoadAt(*seq[p + 41], k)
                || !intLoadAt(*seq[p + 42], t) || seq[p + 43]->op != Op::INEG
                || seq[p + 44]->op != Op::IUSHR || seq[p + 45]->op != Op::IOR
                || !intStoreAt(*seq[p + 46], k)) continue;
            if (!longLoadAt(*seq[p + 47], l) || !intLoadAt(*seq[p + 48], t)
                || seq[p + 49]->op != Op::I2L || seq[p + 50]->op != Op::LXOR) continue;
            // desc ([CJI) 决定 key=3、seed=1（static 方法 0=[C，1-2=J，3=I）。
            if (k != knowledge::kGruntDecryptorKeySlot
                || l != knowledge::kGruntDecryptorSeedSlot) continue;
            cores.emplace_back(st, i, k, l, t);
        }
    }
    if (cores.empty()) {
        why = "解密器核窗口缺失（体非 Grunt 生成面逐字形）";
        return false;
    }
    if (cores.size() > 1) {
        why = "解密器核窗口歧义（多处命中，fail-closed 不猜）";
        return false;
    }

    const CoreSlots& core  = cores.front();
    const ca::u32    sSlot = std::get<0>(core);
    const ca::u32    iSlot = std::get<1>(core);
    const ca::u32    kSlot = std::get<2>(core);

    // ---- init 窗口（6 条）：<int classKey>; iload k; ixor; istore s;
    //      iconst_0; istore i，槽位与核窗口核对，int 常量即 classKey ----
    bool        initFound = false;
    std::optional<ca::i32> initKey;
    for (ca::usize p = 0; p + 6 <= seq.size(); ++p) {
        ca::i32 c = 0;
        if (!const_args::decodeConstInt(s, *seq[p], c)) continue;
        if (!intLoadAt(*seq[p + 1], kSlot) || seq[p + 2]->op != Op::IXOR
            || !intStoreAt(*seq[p + 3], sSlot) || seq[p + 4]->op != Op::ICONST_0
            || !intStoreAt(*seq[p + 5], iSlot)) continue;
        if (initFound && initKey != c) {
            why = "解密器 init 窗口歧义（classKey 多解，fail-closed 不猜）";
            return false;
        }
        initFound = true;
        initKey   = c;
    }
    if (!initFound) {
        why = "解密器 init 窗口缺失（classKey 不在场）";
        return false;
    }

    // ---- 循环头窗口（4 条）：iload i; aload 0; arraylength; if_icmpge，
    //      循环界 = 数组长（keystream 覆盖全长的事实钉面），至少一处 ----
    bool headFound = false;
    for (ca::usize p = 0; p + 4 <= seq.size(); ++p) {
        if (intLoadAt(*seq[p], iSlot) && arrayLoadZeroAt(*seq[p + 1])
            && seq[p + 2]->op == Op::ARRAYLENGTH && seq[p + 3]->op == Op::IF_ICMPGE) {
            headFound = true;
            break;
        }
    }
    if (!headFound) {
        why = "解密器循环头窗口缺失（bound≠数组长的形状不折）";
        return false;
    }

    classKey = *initKey;
    return true;
}

/// @brief arrayInit 块序列解析：整段直线（无标签/无异常表/无分支）分解为
///        N 个 store 块 + 收尾 RETURN，逐条钉形（StringEncryptTransformer /
///        StringArrayedEncrypt 的 INSTRUCTIONS 块逐字对齐）。
/// @return 是否全过（失败原因进 why；解密器名进 decName）。
bool parseArrayInit(ir::ClassSession& s, const PromotedCode& code,
                    const std::string& poolFieldName,
                    std::unordered_map<ca::i32, PoolEntry>& entries, std::string& decName,
                    std::string& why)
{
    const std::vector<const ir::Insn*>& seq = code.seq;

    // 直线生成面：无标签（合并点）、无异常表、无分支指令。
    for (ca::usize p = 0; p < seq.size(); ++p) {
        if (!code.cu->dom.labelsAt(code.cu->dom.order()[p]).empty()) {
            why = "arrayInit 含标签（非直线生成面）";
            return false;
        }
    }
    if (!code.cu->exc.empty()) {
        why = "arrayInit 带异常表（非直线生成面）";
        return false;
    }
    const auto isBranch = [](const ir::Insn& in) {
        switch (in.op) {
        case Op::GOTO:
        case Op::GOTO_W:
        case Op::IF_ICMPEQ:
        case Op::IF_ICMPNE:
        case Op::IF_ICMPLT:
        case Op::IF_ICMPGE:
        case Op::IF_ICMPGT:
        case Op::IF_ICMPLE:
        case Op::IFEQ:
        case Op::IFNE:
        case Op::IFLT:
        case Op::IFGE:
        case Op::IFGT:
        case Op::IFLE:
        case Op::IF_ACMPEQ:
        case Op::IF_ACMPNE:
        case Op::IFNULL:
        case Op::IFNONNULL:
        case Op::TABLESWITCH:
        case Op::LOOKUPSWITCH:
        case Op::JSR:
        case Op::JSR_W:
        case Op::RET:
        case Op::ATHROW: return true;
        default: return false;
        }
    };
    for (const ir::Insn* in : seq) {
        if (isBranch(*in)) {
            why = "arrayInit 含分支指令（非直线生成面）";
            return false;
        }
    }
    if (seq.empty() || seq.back()->op != Op::RETURN) {
        why = "arrayInit 尾条非 RETURN";
        return false;
    }

    const std::string selfName(s.unit().symbols().str(s.unit().thisClass));
    ca::usize         p = 0;
    while (p + 1 < seq.size()) {
        // ---- 块头：GETSTATIC pool（钉回同一池字段）----
        const ir::Insn* head = seq[p];
        if (head->op != Op::GETSTATIC) {
            why = "arrayInit 块头非 GETSTATIC";
            return false;
        }
        ir::ClassSession::MemberSem sem;
        if (!s.memberOf(*head, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field
            || sem.desc != knowledge::kGruntPoolFieldDesc || sem.owner != selfName
            || sem.name != poolFieldName) {
            why = "arrayInit 块头 getstatic 与池字段不符";
            return false;
        }
        ++p;
        // ---- 下标 ----
        ca::i32 idx = 0;
        if (!const_args::decodeConstInt(s, *seq[p], idx) || idx < 0) {
            why = "arrayInit 下标非常量/负值";
            return false;
        }
        ++p;
        // ---- 密文（literal / arrayed 两形态）----
        PoolEntry entry;
        // 形态判别按常量 KIND（v2/v3 的差异是密文形态配置，不是版本键）：
        // String 常量 → literal（v2 ldc+toCharArray 直存形）；int 常量 →
        // arrayed 长度（v3 NEWARRAY T_CHAR 直存码元形；len 经 iconst/bipush/
        // ldc 任意装载，归一后统一 const_args::decodeConstInt 读）。
        ir::ClassSession::ConstSem cs;
        const bool stringCipher = (seq[p]->op == Op::LDC || seq[p]->op == Op::LDC_W)
                                && s.constOf(*seq[p], cs)
                                && cs.kind == ir::ClassSession::ConstSem::Kind::String;
        if (stringCipher) {
            if (!mutf8ToU16(cs.str, entry.cipher)) {
                why = "literal 密文 MUTF-8 解码失败";
                return false;
            }
            entry.form = CipherForm::Literal;
            ++p;
            const ir::Insn* toChar = seq[p];
            ir::ClassSession::MemberSem msem;
            if (toChar->op != Op::INVOKEVIRTUAL || !s.memberOf(*toChar, msem)
                || msem.kind != ir::ClassSession::MemberSem::Kind::Method
                || msem.owner != knowledge::kGruntToCharArrayOwner
                || msem.name != knowledge::kGruntToCharArrayName
                || msem.desc != knowledge::kGruntToCharArrayDesc) {
                why = "literal 密文缺 toCharArray 形";
                return false;
            }
            ++p;
        } else {
            ca::i32 len = 0;
            if (!const_args::decodeConstInt(s, *seq[p], len) || len < 0) {
                why = "arrayed 密文长度非常量/负值";
                return false;
            }
            ++p;
            if (seq[p]->op != Op::NEWARRAY || seq[p]->a != knowledge::kGruntNewarrayTChar) {
                why = "arrayed 密文缺 NEWARRAY T_CHAR";
                return false;
            }
            ++p;
            entry.cipher.resize(static_cast<ca::usize>(len));
            entry.form = CipherForm::Arrayed;
            for (ca::i32 e = 0; e < len; ++e) {
                if (seq[p]->op != Op::DUP) {
                    why = "arrayed 密文缺 DUP（码元写序列断链）";
                    return false;
                }
                ++p;
                ca::i32 slot = 0;
                if (!const_args::decodeConstInt(s, *seq[p], slot) || slot != e) {
                    why = "arrayed 码元下标非常量/乱序";
                    return false;
                }
                ++p;
                ca::i32 code = 0;
                if (!const_args::decodeConstInt(s, *seq[p], code) || code < 0 || code > 0xFFFF) {
                    why = "arrayed 码元值非常量/越界";
                    return false;
                }
                ++p;
                if (seq[p]->op != Op::CASTORE) {
                    why = "arrayed 码元缺 CASTORE";
                    return false;
                }
                ++p;
                entry.cipher[static_cast<ca::usize>(e)] = static_cast<ca::u16>(code);
            }
        }
        // ---- seed(long) ----
        if (!const_args::decodeConstLong(s, *seq[p], entry.seed)) {
            why = "arrayInit seed 非常量";
            return false;
        }
        ++p;
        // ---- key(int) ----
        if (!const_args::decodeConstInt(s, *seq[p], entry.key)) {
            why = "arrayInit key 非常量";
            return false;
        }
        ++p;
        // ---- 解密器引用 ----
        const ir::Insn* invoke = seq[p];
        ir::ClassSession::MemberSem msem;
        if (invoke->op != Op::INVOKESTATIC || !s.memberOf(*invoke, msem)
            || msem.kind != ir::ClassSession::MemberSem::Kind::Method
            || msem.owner != selfName || msem.desc != knowledge::kGruntDecryptorDesc) {
            why = "arrayInit 解密器引用形不符";
            return false;
        }
        if (decName.empty()) {
            decName = std::string(msem.name);
        } else if (decName != msem.name) {
            why = "arrayInit 引用了多个解密器（fail-closed 不猜）";
            return false;
        }
        ++p;
        // ---- aastore 收尾 ----
        if (seq[p]->op != Op::AASTORE) {
            why = "arrayInit 块尾缺 AASTORE";
            return false;
        }
        ++p;
        if (!entries.emplace(idx, std::move(entry)).second) {
            why = "arrayInit 下标重复";
            return false;
        }
    }
    return true;
}

/// @brief 池链识别：clinit 前导（前导四连）→ arrayInit 块解析 → 解密器
///        三窗口 → 池表解密。@return 失败桶（None = 成功，out 已填充）；
///        失败原因进 why。
ChainFail buildPoolChain(ir::ClassSession& s, PoolChain& out, std::string& why)
{
    const ir::ClassUnit& unit     = s.unit();
    const std::string    selfName(unit.symbols().str(unit.thisClass));
    const ca::usize      clinitIdx = findMethodSlot(unit, "<clinit>", "()V");
    if (clinitIdx == kNoSlot) {
        why = "无 <clinit>";
        return ChainFail::NoPrelude;
    }

    PromotedCode clinit;
    if (!promoteToSeq(s, clinitIdx, clinit)) {
        why = "<clinit> promote 失败";
        return ChainFail::NoPrelude;
    }

    // ---- 前导四连：<int n>; ANEWARRAY java/lang/String; PUTSTATIC <pool>;
    //      INVOKESTATIC <arrayInit> ()V，允许多个候选但只取唯一解 ----
    const std::vector<const ir::Insn*>& seq = clinit.seq;
    std::optional<std::string>          poolField;
    std::optional<std::string>          arrayInitName;
    for (ca::usize p = 0; p + 4 <= seq.size(); ++p) {
        ca::i32 poolLen = 0;
        if (!const_args::decodeConstInt(s, *seq[p], poolLen) || poolLen <= 0) continue;
        if (seq[p + 1]->op != Op::ANEWARRAY
            || s.classOf(*seq[p + 1]) != knowledge::kGruntPoolElemType) continue;
        ir::ClassSession::MemberSem fsem;
        if (seq[p + 2]->op != Op::PUTSTATIC || !s.memberOf(*seq[p + 2], fsem)
            || fsem.kind != ir::ClassSession::MemberSem::Kind::Field
            || fsem.desc != knowledge::kGruntPoolFieldDesc || fsem.owner != selfName) continue;
        ir::ClassSession::MemberSem msem;
        if (seq[p + 3]->op != Op::INVOKESTATIC || !s.memberOf(*seq[p + 3], msem)
            || msem.kind != ir::ClassSession::MemberSem::Kind::Method
            || msem.desc != "()V" || msem.owner != selfName) continue;

        // 池字段必须真实在场且 static（防 CP 悬垂引用）。
        bool fieldOk = false;
        for (const ir::FieldUnit& fu : unit.fields) {
            if (unit.symbols().str(fu.name) == fsem.name
                && unit.symbols().str(fu.desc) == knowledge::kGruntPoolFieldDesc
                && (fu.access & kAccStatic) != 0) {
                fieldOk = true;
                break;
            }
        }
        if (!fieldOk) continue;

        if (poolField.has_value() && (*poolField != fsem.name || *arrayInitName != msem.name)) {
            why = "多个池分配前导（fail-closed 不猜）";
            return ChainFail::NoPrelude;
        }
        poolField     = std::string(fsem.name);
        arrayInitName = std::string(msem.name);
        out.poolLen   = poolLen;
    }
    if (!poolField.has_value()) {
        why = "无池分配前导";
        return ChainFail::NoPrelude;
    }
    out.fieldName = *poolField;

    // ---- arrayInit 块序列解析 ----
    // 前导引用的 ()V 可能是直线蹦床：Grunteon 全链产出会把 clinit 的
    // arrayInit 调用藏进自家 indy 机器（boar 语料：helper 体 = [invokedynamic;
    // return]，上游 IndyDecode/Era 还原后成为 [invokestatic <ai>; return]）。
    // 这里只跟**纯结构**的单跳静态蹦床（体恰为 INVOKESTATIC <self> ()V +
    // RETURN，无标签无分支），深度封 2 防；indy 形态 fail-closed 报独立
    // 原因，不猜。
    struct ArrayInitResolve
    {
        std::optional<PromotedCode> code;
    };
    const std::function<ArrayInitResolve(const std::string&, std::string&, int)>
        resolveArrayInit = [&](const std::string& name, std::string& whyOut,
                               int depth) -> ArrayInitResolve {
        if (depth > 2) {
            whyOut = "arrayInit 蹦床深度超限（fail-closed 不猜）";
            return {};
        }
        const ca::usize idx = findMethodSlot(unit, name, "()V");
        if (idx == kNoSlot) {
            whyOut = "前导引用的 arrayInit 不在场";
            return {};
        }
        if ((unit.methods[idx].access & kAccStatic) == 0) {
            whyOut = "arrayInit 非 static";
            return {};
        }
        PromotedCode pc;
        if (!promoteToSeq(s, idx, pc)) {
            whyOut = "arrayInit promote 失败";
            return {};
        }
        // 单跳静态蹦床识别（直线两条件，目标不能是自己防环）。
        if (pc.seq.size() == 2 && pc.seq[0]->op == Op::INVOKESTATIC
            && pc.seq[1]->op == Op::RETURN) {
            ir::ClassSession::MemberSem msem;
            if (s.memberOf(*pc.seq[0], msem)
                && msem.kind == ir::ClassSession::MemberSem::Kind::Method
                && msem.owner == selfName && msem.desc == "()V"
                && msem.name != name) {
                return resolveArrayInit(std::string(msem.name), whyOut, depth + 1);
            }
        }
        return {std::move(pc)};
    };
    std::string resolveWhy;
    ArrayInitResolve resolved = resolveArrayInit(*arrayInitName, resolveWhy, 0);
    if (!resolved.code.has_value()) {
        why = std::move(resolveWhy);
        return ChainFail::ArrayInit;
    }
    PromotedCode arrayInit = std::move(*resolved.code);
    if (!arrayInit.seq.empty() && arrayInit.seq[0]->op == Op::INVOKEDYNAMIC) {
        // 直线蹦床的 indy 形态：调用目标藏在 BSM 里（本仓判据面不评 BSM）。
        if (arrayInit.seq.size() == 2 && arrayInit.seq[1]->op == Op::RETURN) {
            why = "arrayInit 调用被 indy 蹦床隐藏（fail-closed，需上游 indy 还原）";
        } else {
            why = "arrayInit 块头非 GETSTATIC（含 indy 调用面）";
        }
        return ChainFail::ArrayInit;
    }
    std::string decName;
    if (!parseArrayInit(s, arrayInit, out.fieldName, out.entries, decName, why)) {
        return ChainFail::ArrayInit;
    }

    // ---- 解密器：在场、static、三窗口、classKey 提取 ----
    const ca::usize decIdx = findMethodSlot(unit, decName, knowledge::kGruntDecryptorDesc);
    if (decIdx == kNoSlot) {
        why = "解密器缺失（" + decName + " " + std::string(knowledge::kGruntDecryptorDesc)
              + " 不在场）";
        return ChainFail::Decryptor;
    }
    if ((unit.methods[decIdx].access & kAccStatic) == 0) {
        why = "解密器非 static";
        return ChainFail::Decryptor;
    }
    PromotedCode dec;
    if (!promoteToSeq(s, decIdx, dec)) {
        why = "解密器 promote 失败";
        return ChainFail::Decryptor;
    }
    if (!extractClassKey(s, dec, why, out.classKey)) {
        return ChainFail::Decryptor;
    }

    // ---- 池表完整性：块数 = 前导池长（生成面逐字对齐的事实核对）----
    if (static_cast<ca::i32>(out.entries.size()) != out.poolLen) {
        why = "池表块数与前导池长不符";
        return ChainFail::ArrayInit;
    }

    // ---- 逐条解密（S1 定案：参数全装载期常量、解密器核钉形在场）----
    for (auto& [idx, entry] : out.entries) {
        entry.plain = entry.cipher;
        gruntCharXorStream(entry.plain, entry.seed, entry.key, out.classKey);
    }
    return ChainFail::None;
}

/// @brief 单类扫描驱动：池链识别 → 明文记录形编码 → 调用点三元组定案。
///        residual 模式（countOnly=true）只计数不改账（result 仅作草稿）。
void scanClass(ir::ClassSession& s, const std::string& uri, ScanResult& result,
               bool countOnly, ca::usize& residual)
{
    const ir::ClassUnit& unit = s.unit();

    PoolChain   chain;
    std::string why;
    const ChainFail fail = buildPoolChain(s, chain, why);
    if (fail != ChainFail::None) {
        // 失败桶：按原因归类计数（可折叠面缺席不是异常，非本家类占大头）。
        if (!countOnly && fail != ChainFail::NoPrelude) {
            Reject rej;
            rej.uri = uri;
            rej.why = fail == ChainFail::ArrayInit ? "arrayInit 面： " + why
                                                   : "解密器面： " + why;
            result.rejects.push_back(std::move(rej));
        }
        if (fail == ChainFail::ArrayInit) ++result.arrayInitBlocked;
        if (fail == ChainFail::Decryptor) ++result.decryptorBlocked;
        return;
    }
    ++result.poolClasses;

    // ---- 明文记录形编码（孤立代理不可记录 → 该条目退出池表）----
    std::unordered_map<ca::i32, std::pair<CipherForm, std::string>> plainTable;
    for (const auto& [idx, entry] : chain.entries) {
        std::string record;
        if (!u16ToUtf8Record(entry.plain, record)) {
            if (!countOnly) {
                Reject rej;
                rej.uri = uri;
                rej.why = "池条目 " + std::to_string(idx) + " 明文含孤立代理，不可记录（fail-closed）";
                result.rejects.push_back(std::move(rej));
            }
            continue;
        }
        plainTable.emplace(idx, std::make_pair(entry.form, std::move(record)));
    }

    // ---- 调用点三元组：GETSTATIC pool; <int idx>; AALOAD（三连紧邻）----
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (!ir::hasCode(s, mi)) continue;
        auto promoted = ir::promoteMethod(s, mi);
        if (promoted.is_err()) continue;
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;

        const std::vector<ir::InsnId>& order = cu->dom.order();
        if (order.size() < 3) continue;
        const std::string selfName(unit.symbols().str(unit.thisClass));
        for (ca::usize p = 0; p + 2 < order.size(); ++p) {
            const ir::Insn* head = cu->dom.peek(order[p]);
            if (head == nullptr || head->op != Op::GETSTATIC) continue;
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*head, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field
                || sem.desc != knowledge::kGruntPoolFieldDesc || sem.owner != selfName
                || sem.name != chain.fieldName) continue;

            // 合并点检查：三元组内部有标签 = 外部到达路径的栈形状不可知。
            // NOP 跳过（squeeze 遗留落点不驻栈，number_fold 同口径）。
            bool            labeled = false;
            ca::usize       midQ = 0, tailQ = 0;
            const ir::Insn* mid  = nextRealInsn(cu->dom, order, p + 1, &midQ, &labeled);
            const ir::Insn* tail =
                (mid == nullptr) ? nullptr : nextRealInsn(cu->dom, order, midQ + 1, &tailQ, &labeled);
            ca::i32         idx  = 0;
            if (labeled || mid == nullptr || tail == nullptr || !const_args::decodeConstInt(s, *mid, idx)
                || tail->op != Op::AALOAD) {
                if (labeled && !countOnly) {
                    Reject rej;
                    rej.uri         = uri;
                    rej.methodIndex = mi;
                    rej.anchorSrcPc = head->srcPc;
                    rej.why         = "三元组含合并点标签（fail-closed）";
                    result.rejects.push_back(std::move(rej));
                }
                continue;
            }
            ++result.tripleHits;

            const auto it = plainTable.find(idx);
            if (it == plainTable.end()) {
                ++result.idxMissing;
                if (!countOnly) {
                    Reject rej;
                    rej.uri         = uri;
                    rej.methodIndex = mi;
                    rej.anchorSrcPc = head->srcPc;
                    rej.why         = "下标 " + std::to_string(idx) + " 不在池表（fail-closed）";
                    result.rejects.push_back(std::move(rej));
                }
                continue;
            }
            if (countOnly) {
                ++residual;
                continue;
            }
            Site site;
            site.uri         = uri;
            site.methodIndex = mi;
            site.anchorSrcPc = head->srcPc;
            site.owner       = selfName;
            site.fieldName   = chain.fieldName;
            site.index       = idx;
            site.form        = it->second.first;
            site.plainUnits  = chain.entries.at(idx).plain;
            site.plaintext   = it->second.second;
            result.sites.push_back(std::move(site));
        }
    }
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    static_cast<void>(input);
    ScanResult result;
    ca::usize  sink = 0;   // residual 出参：非 count 模式不写，占位保持签名同形
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        scanClass(*session, uri, result, false, sink);
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

namespace {

/// @brief 明文 intern 进常量池：码元 → MUTF-8 → Utf8（命中既有池不膨胀）+
///        CONSTANT_String 包装。@return String 条目索引；0 = 失败（占位槽
///        索引天然是哨兵）。
ca::u32 internPlainString(jbc::core::ConstantPool& cp, const PlainUnits& plain)
{
    std::string mutf8;
    u16ToMutf8(plain, mutf8);
    if (mutf8.empty()) return 0;
    ca::u32 utf8Idx = cp.findUtf8(mutf8);
    if (utf8Idx == 0) {
        utf8Idx = cp.addUtf8(mutf8);
        if (utf8Idx == 0) return 0;
    }
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        const jbc::core::ConstantPool::Entry& e = cp.at(i);
        if (e.tag == CpTag::String && e.a == utf8Idx) return i;
    }
    return cp.addString(utf8Idx);
}

/// @brief 单站点改写。@return false = 未动（failed 计数）。
///
/// 重定位（srcPc + GETSTATIC）→ 形状复核（scan 之后字节可能已被同方法前站
/// 改写，不盲改）→ CP intern → 锚点原地换 ldc（srcPc 保留）→ 跨度其余指令
/// 擦除（eraseBatch kRejectIfUsed 拒收即回滚锚点，不留半改写态）。
bool rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu, const Site& site)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    const std::string selfName(s.unit().symbols().str(s.unit().thisClass));

    ca::usize anchorPos = kNoSlot;
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const ir::Insn* in = cu.dom.peek(order[pos]);
        if (in != nullptr && in->op == Op::GETSTATIC && in->srcPc == site.anchorSrcPc) {
            anchorPos = pos;
            break;
        }
    }
    if (anchorPos == kNoSlot) return false;

    // 形状复核：三元组重钉（owner/字段/下标身份 + 无合并点；NOP 跳过与
    // scan 同口径，跨度内被压指令一并入受害者清单）。
    bool            labeled = false;
    ca::usize       midQ = 0, tailQ = 0;
    const ir::Insn* head = cu.dom.peek(order[anchorPos]);
    const ir::Insn* mid  = nextRealInsn(cu.dom, order, anchorPos + 1, &midQ, &labeled);
    const ir::Insn* tail =
        (mid == nullptr) ? nullptr : nextRealInsn(cu.dom, order, midQ + 1, &tailQ, &labeled);
    if (head == nullptr || mid == nullptr || tail == nullptr || labeled) return false;
    ir::ClassSession::MemberSem sem;
    ca::i32                     idx = 0;
    if (!s.memberOf(*head, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Field
        || sem.owner != selfName || sem.name != site.fieldName
        || sem.desc != knowledge::kGruntPoolFieldDesc || !const_args::decodeConstInt(s, *mid, idx)
        || idx != site.index || tail->op != Op::AALOAD) {
        return false;
    }

    // 明文 MUTF-8 编码 + CP intern：失败面在动 DOM 之前收口。
    const ca::u32 strIdx = internPlainString(s.backing().cp, site.plainUnits);
    if (strIdx == 0) return false;

    // 锚点原地换 ldc（srcPc 保留：pcmap/调试行落点不变）。
    auto slotR = cu.dom.get(order[anchorPos]);
    if (slotR.is_err()) return false;
    ir::Insn*     anchor   = slotR.unwrap();
    const ir::Insn original = *anchor;

    ir::Insn fresh = ir::CodeEditor::loadConst(strIdx);
    fresh.srcPc    = original.srcPc;
    *anchor        = fresh;

    std::vector<ir::InsnId> victims;
    victims.reserve(tailQ - anchorPos);
    for (ca::usize q = anchorPos + 1; q <= tailQ; ++q) victims.push_back(order[q]);
    if (!cu.dom.eraseBatch(victims).is_ok()) {
        *anchor = original;
        return false;
    }
    return true;
}

}   // namespace

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    // 站点按 uri 聚类（万类大 jar 上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit = 0;
        ca::usize           literal = 0;
        ca::usize           arrayed = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        const auto        it = byUri.find(uri);
        if (it == byUri.end()) return;
        const std::vector<const Site*>& sitesOf = it->second;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        // 前置检查：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit     = 0;
        ca::usize failed  = 0;
        ca::usize literal = 0;
        ca::usize arrayed = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit   = 0;
            ca::usize methodFail  = 0;
            ca::usize methodLit   = 0;
            ca::usize methodArr   = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    for (const Site* site : sitesOfMethod) {
                        if (rewriteSite(s, cu, *site)) {
                            ++methodHit;
                            if (site->form == CipherForm::Literal) ++methodLit;
                            else ++methodArr;
                        } else {
                            ++methodFail;
                        }
                    }
                    return methodHit > 0;
                });
            if (!ok) {
                // 帧链失败：该方法整方法放弃（session 内半改写态不外泄）。
                failed += methodHit + methodFail;
                continue;
            }
            hit += methodHit;
            failed += methodFail;
            literal += methodLit;
            arrayed += methodArr;
        }
        dirty.failed += failed;
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit, literal, arrayed});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.hit;
        dirty.classes += 1;
        dirty.foldedLiteral += item.literal;
        dirty.foldedArrayed += item.arrayed;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    // proof 是效力检查（不变量复核），不许被 pass 级时间片截短，挂起期间
    // forEachClass 全量放行（residual 统计同样要看到全量类）。
    pass_clock::Suspend                   suspend;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;

    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ScanResult counter;
        counter.tripleHits = 0;   // residual 模式不写共享账
        scanClass(*session, uri, counter, true, residual);
    });
    result.residual = residual;
    return result;
}

}   // namespace grunt::string_pool_fold
