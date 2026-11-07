/// @file        material_key_restore.cpp
/// @brief       残留 `material_key_restore`：RuntimeMaterial 材料通道 key 静态
///              折回（scan / rewrite / proof 三段，见 .hpp 形态与口径）。
#include "grunt/material_key_restore.hpp"

#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "libca/core/datatype.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "jbc/type/descriptor.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/pass_clock.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/value.hpp"

namespace grunt::material_key_restore {

namespace {

namespace ir = jbc::ir;
using jbc::core::CpTag;
using jbc::core::Op;

constexpr ca::usize kNoSlot     = static_cast<ca::usize>(-1);
constexpr ca::u32   kAccPrivate = 0x0002;
constexpr ca::u32   kAccStatic  = 0x0008;

// ============================================================================
// ==== 形状钉面辅助（常量装载读数走 const_args，槽装载仍就地钉形）============
// ============================================================================

/// @brief int 槽装载（ILOAD a==slot 或 ILOAD_0..3 快捷形）。
bool intLoadAt(const ir::Insn& in, ca::u32 slot)
{
    if (in.op == Op::ILOAD) return in.a == static_cast<ca::i32>(slot);
    return in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3
        && slot <= 3
        && static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ILOAD_0)) == slot;
}

/// @brief 引用槽装载（ALOAD a==slot 或 ALOAD_0..3 快捷形）。
bool refLoadAt(const ir::Insn& in, ca::u32 slot)
{
    if (in.op == Op::ALOAD) return in.a == static_cast<ca::i32>(slot);
    return in.op >= Op::ALOAD_0 && in.op <= Op::ALOAD_3
        && slot <= 3
        && static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ALOAD_0)) == slot;
}

/// @brief 分支类指令（窗口/跨度内的合并与控制流禁止面）。
bool branchyAt(const ir::Insn& in)
{
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
}

/// @brief 方法骨架定位：按名字 + 描述符在 unit.methods 找唯一槽。
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
// ==== 码元面编解码走 scan_toolbox（mutf8ToUnits / decryptXor）================
// ============================================================================

/// @brief `Long.foldToInt`（ReferenceObfuscate 源面逐字）：
///        `(seed ^ (seed ushr 32)).toInt()`。
ca::i32 materialFold(ca::i64 seed) noexcept
{
    const ca::u64 x = static_cast<ca::u64>(seed);
    return static_cast<ca::i32>(static_cast<ca::u32>(x ^ (x >> 32)));
}

/// @brief `Long.parseUnsignedLong(s, radix)` 严格转写：无符号、无符号前缀
///        （'-' 本方法直接拒；'+' 亦不在实证生成面，fail-closed）、溢出拒。
///        码元必须全 ASCII 数字（Character.digit 的 ASCII 子集，两 case 都收）。
bool parseRadixDigits(const std::vector<ca::u16>& units, ca::i32 radix, ca::u64& out)
{
    if (units.empty() || radix < 2 || radix > 36) return false;
    const ca::u64 radixU = static_cast<ca::u64>(radix);
    const ca::u64 limit  = std::numeric_limits<ca::u64>::max() / radixU;
    ca::u64       v      = 0;
    for (const ca::u16 u : units) {
        if (u < '0' || u > 'z') return false;
        ca::u64 d = 0;
        if (u <= '9') {
            d = static_cast<ca::u64>(u - '0');
        } else if (u >= 'a') {
            d = static_cast<ca::u64>(u - 'a') + 10;
        } else if (u >= 'A' && u <= 'Z') {
            d = static_cast<ca::u64>(u - 'A') + 10;
        } else {
            return false;
        }
        if (d >= radixU) return false;
        if (v > limit || v * radixU > (std::numeric_limits<ca::u64>::max() - d)) return false;
        v = v * radixU + d;
    }
    out = v;
    return true;
}

// ============================================================================
// ==== 方法视图（每方法一次 promote 的会话级缓存）============================
// ============================================================================

/// @brief 一次 promote 的会话级产物（dom 序非空指令视图 + cu 借用）。
struct MethodView
{
    std::vector<const ir::Insn*> seq;   ///< dom 序非空指令视图
    ir::CodeUnit*                cu = nullptr;
};

/// @brief 全类 promote（只填缺席条目——同一会话内每方法恰一次，遵守 promote
///        纪律；重复调用安全）。@return false = 存在 promote 失败的方法
///        （其视图缺席；写者普查面调用方必须按 fail-closed 处理）。
bool promoteAllInto(ir::ClassSession& s, std::unordered_map<ca::usize, MethodView>& out)
{
    bool allOk = true;
    const ir::ClassUnit& unit = s.unit();
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (out.count(mi) != 0) continue;
        if (!ir::hasCode(s, mi)) continue;
        auto promoted = ir::promoteMethod(s, mi);
        if (promoted.is_err()) {
            allOk = false;
            continue;
        }
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) {
            allOk = false;
            continue;
        }
        MethodView view;
        view.cu = cu;
        view.seq.reserve(cu->dom.order().size());
        bool seqOk = true;
        for (const ir::InsnId id : cu->dom.order()) {
            const ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) {
                seqOk = false;
                break;
            }
            view.seq.push_back(in);
        }
        if (!seqOk) {
            allOk = false;
            continue;
        }
        out.emplace(mi, std::move(view));
    }
    return allOk;
}

/// @brief 成员语义读取（字段/方法；失败 false）。
bool memberOf(const ir::ClassSession& s, const ir::Insn& in,
              ir::ClassSession::MemberSem::Kind kind, ir::ClassSession::MemberSem& sem)
{
    if (!s.memberOf(in, sem)) return false;
    return sem.kind == kind;
}

// ============================================================================
// ==== 材料解密器窗口（scan 与 proof 共用的唯一形状面）=======================
// ============================================================================

/// @brief 一个材料解密器窗口（I2C ← IXOR ← IXOR ← INVOKESTATIC <self>()I ←
///        LDC K，回溯口径与 indy_restore 的 scanDecryptKey 同形）。
struct MaterialWindow
{
    ca::usize   decryptorIndex = kNoSlot;  ///< (String;)String 解密器方法槽
    std::string decryptorName;             ///< 解密器方法名（BSM 同名配对面）
    std::string materialKeyName;           ///< INVOKESTATIC 目标（materialKey() 名）
    ca::usize   invokePos     = 0;         ///< INVOKESTATIC 在解密器 seq 里的位置
    ca::u32     invokeSrcPc   = 0;         ///< INVOKESTATIC 旧布局 pc
    ca::i32     rawKey        = 0;         ///< LDC K（异或前原始 key）
};

/// @brief 窗口扫描（纯读）：对一类的全部 (String;)String 方法跑回溯钉形。
///        一类多窗口时全量返回（调用方按歧义处理）。
std::vector<MaterialWindow> findMaterialWindows(
    ir::ClassSession& s, const std::unordered_map<ca::usize, MethodView>& views)
{
    std::vector<MaterialWindow> out;
    const ir::ClassUnit&        unit = s.unit();
    const std::string           self(unit.symbols().str(unit.thisClass));

    for (const auto& [mi, view] : views) {
        const std::string_view desc = unit.symbols().str(unit.methods[mi].desc);
        if (desc != knowledge::kGruntSimpleDecryptorDesc) continue;
        const std::vector<const ir::Insn*>& seq = view.seq;
        if (seq.size() < 5) continue;
        for (ca::usize p = 4; p < seq.size(); ++p) {
            if (seq[p]->op != Op::I2C) continue;
            if (seq[p - 1]->op != Op::IXOR || seq[p - 2]->op != Op::IXOR) continue;
            if (seq[p - 3]->op != Op::INVOKESTATIC) continue;
            ir::ClassSession::MemberSem sem;
            if (!memberOf(s, *seq[p - 3], ir::ClassSession::MemberSem::Kind::Method, sem)) {
                continue;
            }
            if (sem.desc != "()I" || sem.owner != self) continue;
            ca::i32 raw = 0;
            if (!const_args::decodeConstInt(s, *seq[p - 4], raw)) continue;
            MaterialWindow w;
            w.decryptorIndex = mi;
            w.decryptorName  = std::string(unit.symbols().str(unit.methods[mi].name));
            w.materialKeyName = std::string(sem.name);
            w.invokePos       = p - 3;
            w.invokeSrcPc     = seq[p - 3]->srcPc;
            w.rawKey          = raw;
            out.push_back(std::move(w));
        }
    }
    return out;
}

// ============================================================================
// ==== materialKey() 体形 + 材料字段 =========================================
// ============================================================================

/// @brief materialKey() 体形（createMaterialKeyMethod 逐字 9 条）与字段身份。
struct MaterialKeyBody
{
    std::string keyField;     ///< GETSTATIC keyField(J) 的字段名
    std::string poisonField;  ///< GETSTATIC poisonField(I) 的字段名
};

/// @brief 9 条逐字钉形：GETSTATIC F_k(J); DUP2; INT(32); LUSHR; LXOR; L2I;
///        GETSTATIC F_p(I); IXOR; IRETURN。无标签（首条外）、无异常表；字段
///        private static 在场。@return nullopt = 体形/字段不符。
std::optional<MaterialKeyBody> matchMaterialKeyBody(
    ir::ClassSession& s, const std::unordered_map<ca::usize, MethodView>& views,
    const std::string& mkName)
{
    const ir::ClassUnit& unit = s.unit();
    const std::string    self(unit.symbols().str(unit.thisClass));
    const ca::usize      mi = findMethodSlot(unit, mkName, "()I");
    if (mi == kNoSlot) return std::nullopt;
    if ((unit.methods[mi].access & (kAccStatic | kAccPrivate))
        != (kAccStatic | kAccPrivate)) {
        return std::nullopt;
    }
    const auto it = views.find(mi);
    if (it == views.end()) return std::nullopt;
    const std::vector<const ir::Insn*>& seq = it->second.seq;
    if (seq.size() != 9) return std::nullopt;
    if (!it->second.cu->exc.empty()) return std::nullopt;
    for (ca::usize p = 1; p < 9; ++p) {
        if (!it->second.cu->dom.labelsAt(it->second.cu->dom.order()[p]).empty()) {
            return std::nullopt;
        }
    }

    ir::ClassSession::MemberSem kSem;
    ir::ClassSession::MemberSem pSem;
    if (seq[0]->op != Op::GETSTATIC
        || !memberOf(s, *seq[0], ir::ClassSession::MemberSem::Kind::Field, kSem)
        || kSem.owner != self || kSem.desc != "J") {
        return std::nullopt;
    }
    ca::i32 shift = 0;
    if (seq[1]->op != Op::DUP2 || !const_args::decodeConstInt(s, *seq[2], shift)
        || shift != static_cast<ca::i32>(knowledge::kGruntMaterialKeySlotShift)
        || seq[3]->op != Op::LUSHR || seq[4]->op != Op::LXOR || seq[5]->op != Op::L2I) {
        return std::nullopt;
    }
    if (seq[6]->op != Op::GETSTATIC
        || !memberOf(s, *seq[6], ir::ClassSession::MemberSem::Kind::Field, pSem)
        || pSem.owner != self || pSem.desc != "I" || seq[7]->op != Op::IXOR
        || seq[8]->op != Op::IRETURN) {
        return std::nullopt;
    }
    // 两字段 private static 在场（防 CP 悬垂引用；引用普查口径宁严勿宽）。
    const auto fieldOk = [&](const std::string& name, std::string_view desc) {
        for (const ir::FieldUnit& fu : unit.fields) {
            if (unit.symbols().str(fu.name) == name && unit.symbols().str(fu.desc) == desc
                && (fu.access & (kAccStatic | kAccPrivate)) == (kAccStatic | kAccPrivate)) {
                return true;
            }
        }
        return false;
    };
    if (!fieldOk(std::string(kSem.name), "J") || !fieldOk(std::string(pSem.name), "I")) {
        return std::nullopt;
    }
    MaterialKeyBody body;
    body.keyField    = std::string(kSem.name);
    body.poisonField = std::string(pSem.name);
    return body;
}

// ============================================================================
// ==== 初值链：写者普查 + 扰动窗口 + 初值求值 ================================
// ============================================================================

/// @brief 一个字段写者普查的中间账。
struct FieldCensus
{
    ca::usize clinitIndex = kNoSlot;
    std::string fieldName;
    std::string fieldDesc;                   ///< "J" / "I"

    std::optional<std::pair<ca::usize, ca::usize>> initAt;  ///< 初值写（方法槽, seq 位置）
    std::vector<ca::usize>  suspiciousMethods;              ///< 扰动写所在方法槽（查守卫指纹用）
    ca::usize               strayWrites   = 0;              ///< clinit 外非窗口形写（writerShape 桶）
};

/// @brief 扰动窗口形回扫：PUTSTATIC F ← LXOR|IOR ← …（≤64 条、无分支、无
///        同字段二次写）… ← GETSTATIC F。头（GETSTATIC F）允许坐在标签上
///        （CFF 摊平块头，boar 实证：守卫 ifeq 的目标块以窗口头开场）；跨度
///        严格内部不允许合并点/分支/同字段二次写。
bool looksSuspiciousWindow(ir::ClassSession& s, const MethodView& view, ca::usize q,
                           const std::string& name, const std::string& desc)
{
    constexpr ca::usize kWindowBound = 64;
    const std::vector<const ir::Insn*>& seq = view.seq;
    if (q == 0) return false;
    if (seq[q - 1]->op != Op::LXOR && seq[q - 1]->op != Op::IOR) return false;
    ir::CodeDom& dom = view.cu->dom;
    const auto   labeledAt = [&](ca::usize p) {
        return !dom.labelsAt(dom.order()[p]).empty();
    };
    for (ca::usize g = q - 1; g + kWindowBound >= q + 1; --g) {
        const ir::Insn* in = seq[g];
        // 头（GETSTATIC 同字段）即刻收口——头坐标签是摊平常态，不排斥。
        if (in->op == Op::GETSTATIC) {
            ir::ClassSession::MemberSem sem;
            if (memberOf(s, *in, ir::ClassSession::MemberSem::Kind::Field, sem)
                && sem.owner == s.unit().symbols().str(s.unit().thisClass)
                && sem.name == name && sem.desc == desc) {
                return true;
            }
        }
        if (g != q - 1) {
            if (labeledAt(g)) return false;             // 跨度内部合并点
            if (branchyAt(*in)) return false;
            if (in->op == Op::PUTSTATIC) {
                ir::ClassSession::MemberSem sem;
                if (memberOf(s, *in, ir::ClassSession::MemberSem::Kind::Field, sem)
                    && sem.name == name && sem.desc == desc) {
                    return false;   // 同字段二次写：非生成面窗口
                }
            }
        }
        if (g == 0) break;
    }
    return false;
}

/// @brief 守卫指纹：`INVOKESTATIC <self> ()I; DUP; IFEQ` 连续三条（RuntimeMaterial
///        perturbation 的分支结构，摊平块内直线在场）。
bool guardFingerprint(ir::ClassSession& s, const MethodView& view)
{
    const std::vector<const ir::Insn*>& seq = view.seq;
    if (seq.size() < 3) return false;
    const std::string self(s.unit().symbols().str(s.unit().thisClass));
    for (ca::usize p = 0; p + 2 < seq.size(); ++p) {
        if (seq[p]->op != Op::INVOKESTATIC) continue;
        ir::ClassSession::MemberSem sem;
        if (!memberOf(s, *seq[p], ir::ClassSession::MemberSem::Kind::Method, sem)
            || sem.owner != self || sem.desc != "()I") {
            continue;
        }
        if (seq[p + 1]->op == Op::DUP && seq[p + 2]->op == Op::IFEQ) return true;
    }
    return false;
}

/// @brief 对字段的全部 PUTSTATIC 站点普查归类（纯读）。@return false = 出现
///        非窗口形的 clinit 外写（writerShape 桶）。
bool censusField(ir::ClassSession& s, const std::unordered_map<ca::usize, MethodView>& views,
                 ca::usize clinitIndex, const std::string& name, const std::string& desc,
                 FieldCensus& out)
{
    const ir::ClassUnit& unit = s.unit();
    const std::string    self(unit.symbols().str(unit.thisClass));
    out.clinitIndex = clinitIndex;
    out.fieldName   = name;
    out.fieldDesc   = desc;

    for (const auto& [mi, view] : views) {
        const std::vector<const ir::Insn*>& seq = view.seq;
        for (ca::usize q = 0; q < seq.size(); ++q) {
            if (seq[q]->op != Op::PUTSTATIC) continue;
            ir::ClassSession::MemberSem sem;
            if (!memberOf(s, *seq[q], ir::ClassSession::MemberSem::Kind::Field, sem)
                || sem.owner != self || sem.name != name || sem.desc != desc) {
                continue;
            }
            if (looksSuspiciousWindow(s, view, q, name, desc)) {
                out.suspiciousMethods.push_back(mi);
                continue;
            }
            if (mi != clinitIndex) {
                ++out.strayWrites;   // clinit 外的非窗口形写：字段被外写
                continue;
            }
            if (out.initAt.has_value()) {
                out.initAt = std::pair<ca::usize, ca::usize>{kNoSlot, kNoSlot};   // 多写冲突哨兵
                continue;
            }
            out.initAt = std::pair<ca::usize, ca::usize>{mi, q};
        }
    }
    return out.strayWrites == 0;
}

/// @brief parse 蹦床体形：`aload_0; iload_1; invokestatic
///        java/lang/Long.parseUnsignedLong (String;I)J; lreturn`（4 条直线，
///        IndyRestore 第一轮解直后的产物形）。@return true = 形与身份全过。
bool parseTrampolineShape(ir::ClassSession& s,
                          std::unordered_map<ca::usize, MethodView>& views,
                          const std::string& name)
{
    const ir::ClassUnit& unit = s.unit();
    const ca::usize      mi   = findMethodSlot(unit, name, knowledge::kGruntMaterialParseDesc);
    if (mi == kNoSlot) return false;
    if ((unit.methods[mi].access & kAccStatic) == 0) return false;
    promoteAllInto(s, views);
    const auto it = views.find(mi);
    if (it == views.end()) return false;
    const std::vector<const ir::Insn*>& seq = it->second.seq;
    if (seq.size() != 4) return false;
    if (!it->second.cu->exc.empty()) return false;
    ir::CodeDom& dom = it->second.cu->dom;
    for (ca::usize p = 1; p < 4; ++p) {
        if (!dom.labelsAt(dom.order()[p]).empty()) return false;
    }
    if (!refLoadAt(*seq[0], 0) || !intLoadAt(*seq[1], 1) || seq[3]->op != Op::LRETURN) {
        return false;
    }
    if (seq[2]->op != Op::INVOKESTATIC) return false;
    ir::ClassSession::MemberSem sem;
    return memberOf(s, *seq[2], ir::ClassSession::MemberSem::Kind::Method, sem)
        && sem.owner == knowledge::kGruntMaterialParseOwner
        && sem.name == knowledge::kGruntMaterialParseName
        && sem.desc == knowledge::kGruntMaterialParseDesc;
}

/// @brief keyField 初值求值。
///        Form A：`LDC2_W v; PUTSTATIC`（生成面原形，number-reobf 前的罐）。
///        Form B：`ldc <明文>; INT(32); INVOKESTATIC <parse 或蹦床>(String;I)J;
///        ldc2_w C; lxor; PUTSTATIC`（boar 实证形：StringPoolFold 折回池明文 +
///        IndyRestore 解直蹦床之后的供给链产物）。
/// @return true = 求值成功（值进 out）；失败时 blockedWhy 带桶内原因。
bool evalKeyInit(ir::ClassSession& s, std::unordered_map<ca::usize, MethodView>& views,
                 const MethodView& clinit, ca::usize q, ca::i64& out, std::string& blockedWhy)
{
    const std::vector<const ir::Insn*>& seq = clinit.seq;
    ir::CodeDom&                        dom = clinit.cu->dom;
    const auto interiorLabeled = [&](ca::usize lo, ca::usize hi) {
        for (ca::usize p = lo + 1; p < hi; ++p) {
            if (!dom.labelsAt(dom.order()[p]).empty()) return true;
        }
        return false;
    };

    // ---- Form A：单条 long 常量装载 ----
    if (q >= 1) {
        ca::i64 v = 0;
        if (const_args::decodeConstLong(s, *seq[q - 1], v)) {
            out = v;
            return true;
        }
    }

    // ---- Form B：`ldc 明文; INT(32); INVOKESTATIC parse; ldc2_w C; lxor` ----
    if (q >= 5 && seq[q - 1]->op == Op::LXOR) {
        ca::i64 cxor = 0;
        if (const_args::decodeConstLong(s, *seq[q - 2], cxor) && seq[q - 3]->op == Op::INVOKESTATIC) {
            ir::ClassSession::MemberSem psem;
            const bool parseDirect =
                memberOf(s, *seq[q - 3], ir::ClassSession::MemberSem::Kind::Method, psem)
                && psem.owner == knowledge::kGruntMaterialParseOwner
                && psem.name == knowledge::kGruntMaterialParseName
                && psem.desc == knowledge::kGruntMaterialParseDesc;
            const bool parseTramp = memberOf(s, *seq[q - 3],
                                             ir::ClassSession::MemberSem::Kind::Method, psem)
                                    && psem.owner
                                           == s.unit().symbols().str(s.unit().thisClass)
                                    && psem.desc == knowledge::kGruntMaterialParseDesc
                                    && parseTrampolineShape(s, views, std::string(psem.name));
            ca::i32 radix = 0;
            if ((parseDirect || parseTramp) && const_args::decodeConstInt(s, *seq[q - 4], radix)
                && radix == knowledge::kGruntMaterialParseRadix
                && (seq[q - 5]->op == Op::LDC || seq[q - 5]->op == Op::LDC_W)
                && !interiorLabeled(q - 5, q)) {
                ir::ClassSession::ConstSem cs;
                if (!s.constOf(*seq[q - 5], cs) || cs.kind != ir::ClassSession::ConstSem::Kind::String) {
                    blockedWhy = "Form B 明文装载非常量串";
                    return false;
                }
                std::vector<ca::u16> units;
                if (!scan_tools::mutf8ToUnits(cs.str, units)) {
                    blockedWhy = "Form B 明文 MUTF-8 解码失败";
                    return false;
                }
                ca::u64 mag = 0;
                if (!parseRadixDigits(units, radix, mag)) {
                    blockedWhy = "parseUnsignedLong 求值失败（空串/坏数字/溢出）";
                    return false;
                }
                out = static_cast<ca::i64>(mag ^ static_cast<ca::u64>(cxor));
                return true;
            }
        }
    }

    // ---- 桶内归因：池读未折回（链序缺 StringPoolFold）是主流形 ----
    for (ca::usize back = 1; back <= 8 && back <= q; ++back) {
        const ir::Insn* in = seq[q - back];
        if (in->op == Op::GETSTATIC) {
            ir::ClassSession::MemberSem sem;
            if (memberOf(s, *in, ir::ClassSession::MemberSem::Kind::Field, sem)
                && sem.desc == knowledge::kGruntPoolFieldDesc) {
                blockedWhy = "初值跨度仍含池读（链序缺 grunt.StringPoolFold，fail-closed）";
                return false;
            }
        }
    }
    blockedWhy = "初值跨度非生成面形（Form A ldc2_w / Form B parseUnsignedLong 都不匹）";
    return false;
}

/// @brief poison 初值求值：纯常量走廊小栈机（int/long 双域、JVM 回绕与移位
///        掩码语义；词表 = 常量装载 + I2L/L2I + 位运算/加减/移位/取负族）。
///        评法裁定见 doc/material-key-restore.impl.md：单方法字面走廊 C++ 直
///        解，不骑 jsim——求值子集不含堆访问/调用/分配，「跑不出值」即
///        fail-closed，两态语义等价；走廊过重（>24 条）同拒。
/// @return true = 求值成功（int 位型进 out；顶型必须 I——PUTSTATIC I 的栈形）。
bool evalPoisonInit(ir::ClassSession& s, const MethodView& clinit, ca::usize q, ca::i32& out)
{
    struct MiniVal
    {
        bool    isLong = false;
        ca::u64 bits   = 0;
    };
    const std::vector<const ir::Insn*>& seq = clinit.seq;
    ir::CodeDom&                        dom = clinit.cu->dom;
    constexpr ca::usize                 kWindowMax = 24;

    const auto tryEval = [&](ca::usize start, ca::u64& result) {
        std::vector<MiniVal> stack;
        const auto           pop = [&](bool wantLong, MiniVal& v) {
            if (stack.empty()) return false;
            v = stack.back();
            if (v.isLong != wantLong) return false;
            stack.pop_back();
            return true;
        };
        const auto pushI = [&](ca::u32 bits) {
            stack.push_back(MiniVal{false, static_cast<ca::u64>(bits)});
        };
        const auto pushJ = [&](ca::u64 bits) {
            stack.push_back(MiniVal{true, bits});
        };
        for (ca::usize p = start; p < q; ++p) {
            if (!dom.labelsAt(dom.order()[p]).empty()) return false;
            const ir::Insn* in = seq[p];
            MiniVal         a;
            MiniVal         b;
            switch (in->op) {
            case Op::ICONST_M1:
            case Op::ICONST_0:
            case Op::ICONST_1:
            case Op::ICONST_2:
            case Op::ICONST_3:
            case Op::ICONST_4:
            case Op::ICONST_5: {
                const ca::i32 v = in->op == Op::ICONST_M1
                                      ? -1
                                      : static_cast<ca::i32>(static_cast<int>(in->op)
                                                             - static_cast<int>(Op::ICONST_0));
                pushI(static_cast<ca::u32>(v));
                break;
            }
            case Op::LCONST_0:
            case Op::LCONST_1:
                pushJ(in->op == Op::LCONST_1 ? 1u : 0u);
                break;
            case Op::BIPUSH:
            case Op::SIPUSH:
                pushI(static_cast<ca::u32>(in->a));
                break;
            case Op::LDC:
            case Op::LDC_W: {
                if (!s.backing().cp.valid(in->cp)) return false;
                const jbc::core::ConstantPool::Entry& e = s.backing().cp.at(in->cp);
                if (e.tag != CpTag::Integer) return false;
                pushI(static_cast<ca::u32>(e.ival));
                break;
            }
            case Op::LDC2_W: {
                if (!s.backing().cp.valid(in->cp)) return false;
                const jbc::core::ConstantPool::Entry& e = s.backing().cp.at(in->cp);
                if (e.tag != CpTag::Long) return false;
                pushJ(static_cast<ca::u64>(e.lval));
                break;
            }
            case Op::I2L: {
                if (!pop(false, a)) return false;
                pushJ(static_cast<ca::u64>(static_cast<ca::i64>(static_cast<ca::i32>(a.bits))));
                break;
            }
            case Op::L2I: {
                if (!pop(true, a)) return false;
                pushI(static_cast<ca::u32>(a.bits));
                break;
            }
            case Op::INEG: {
                if (!pop(false, a)) return false;
                pushI(static_cast<ca::u32>(0u - a.bits));
                break;
            }
            case Op::LNEG: {
                if (!pop(true, a)) return false;
                pushJ(0u - a.bits);
                break;
            }
            case Op::IADD:
            case Op::ISUB:
            case Op::IXOR:
            case Op::IOR:
            case Op::IAND: {
                if (!pop(false, b) || !pop(false, a)) return false;
                ca::u32 r = 0;
                switch (in->op) {
                case Op::IADD: r = static_cast<ca::u32>(a.bits) + static_cast<ca::u32>(b.bits); break;
                case Op::ISUB: r = static_cast<ca::u32>(a.bits) - static_cast<ca::u32>(b.bits); break;
                case Op::IXOR: r = static_cast<ca::u32>(a.bits) ^ static_cast<ca::u32>(b.bits); break;
                case Op::IOR:  r = static_cast<ca::u32>(a.bits) | static_cast<ca::u32>(b.bits); break;
                default:       r = static_cast<ca::u32>(a.bits) & static_cast<ca::u32>(b.bits); break;
                }
                pushI(r);
                break;
            }
            case Op::LADD:
            case Op::LSUB:
            case Op::LXOR:
            case Op::LOR:
            case Op::LAND: {
                if (!pop(true, b) || !pop(true, a)) return false;
                ca::u64 r = 0;
                switch (in->op) {
                case Op::LADD: r = a.bits + b.bits; break;
                case Op::LSUB: r = a.bits - b.bits; break;
                case Op::LXOR: r = a.bits ^ b.bits; break;
                case Op::LOR:  r = a.bits | b.bits; break;
                default:       r = a.bits & b.bits; break;
                }
                pushJ(r);
                break;
            }
            case Op::ISHL:
            case Op::ISHR:
            case Op::IUSHR: {
                if (!pop(false, b) || !pop(false, a)) return false;
                const ca::u32 sh = static_cast<ca::u32>(b.bits) & 31u;
                const ca::u32 va = static_cast<ca::u32>(a.bits);
                ca::u32       r  = 0;
                switch (in->op) {
                case Op::ISHL:  r = va << sh; break;
                case Op::ISHR:  r = static_cast<ca::u32>(static_cast<ca::i32>(va) >> sh); break;
                default:        r = va >> sh; break;
                }
                pushI(r);
                break;
            }
            case Op::LSHL:
            case Op::LSHR:
            case Op::LUSHR: {
                if (!pop(false, b) || !pop(true, a)) return false;
                const ca::u32 sh = static_cast<ca::u32>(b.bits) & 63u;
                ca::u64       r  = 0;
                switch (in->op) {
                case Op::LSHL:  r = a.bits << sh; break;
                case Op::LSHR:  r = static_cast<ca::u64>(static_cast<ca::i64>(a.bits) >> sh); break;
                default:        r = a.bits >> sh; break;
                }
                pushJ(r);
                break;
            }
            default:
                return false;
            }
        }
        if (stack.size() != 1) return false;
        if (stack.front().isLong) return false;   // PUTSTATIC I 的栈顶恒 I
        result = stack.front().bits;
        return true;
    };

    const ca::usize lo = q > kWindowMax ? q - kWindowMax : 0;
    std::optional<ca::u64> value;
    for (ca::usize start = lo; start < q; ++start) {
        ca::u64 r = 0;
        if (!tryEval(start, r)) continue;
        if (value.has_value() && *value != r) return false;   // 多解：不猜
        value = r;
    }
    if (!value.has_value()) return false;
    out = static_cast<ca::i32>(static_cast<ca::u32>(*value));
    return true;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

namespace {

/// @brief 一类的完整分析（窗口 → materialKey 体 → 写者普查 → 初值求值 → S1
///        互证）。@return nullopt = 类级拒绝（拒绝账已记）；否则产出 0/1 站点。
std::optional<Site> analyzeClass(ir::ClassSession& s, const std::string& uri, ScanResult& stats)
{
    const ir::ClassUnit& unit = s.unit();
    const std::string    self(unit.symbols().str(unit.thisClass));

    // ---- 家族锚：无 Grunt simple BSM desc 指纹的类不进面 ----
    const auto* bsms = scan_tools::bsmEntries(s);
    if (bsms == nullptr) return std::nullopt;
    bool familyBsm = false;
    for (const auto& bsm : *bsms) {
        if (!s.backing().cp.valid(bsm.methodRef)) continue;
        const jbc::core::ConstantPool::Entry& mh = s.backing().cp.at(bsm.methodRef);
        if (mh.tag != CpTag::MethodHandle || !s.backing().cp.valid(mh.b)) continue;
        const jbc::core::ConstantPool::Entry& ref = s.backing().cp.at(mh.b);
        if ((ref.tag != CpTag::Methodref && ref.tag != CpTag::InterfaceMethodref)
            || !s.backing().cp.valid(ref.b)) {
            continue;
        }
        const jbc::core::ConstantPool::Entry& nat = s.backing().cp.at(ref.b);
        if (nat.tag != CpTag::NameAndType || !s.backing().cp.valid(nat.b)) continue;
        if (s.backing().cp.utf8Of(nat.b) == knowledge::kGruntSimpleBsmDesc) {
            familyBsm = true;
            break;
        }
    }
    if (!familyBsm) return std::nullopt;

    // ---- 全类 promote（每方法一次；失败不立即拒——先看是否材料类，见下）----
    std::unordered_map<ca::usize, MethodView> views;
    const bool promoteOk = promoteAllInto(s, views);

    // ---- 窗口定位（一类恰一个）----
    std::vector<MaterialWindow> windows = findMaterialWindows(s, views);
    if (windows.empty()) return std::nullopt;
    ++stats.materialClasses;
    if (windows.size() > 1) {
        Reject rej;
        rej.uri         = uri;
        rej.methodIndex = windows.front().decryptorIndex;
        rej.why         = "一类多个材料解密器窗口（fail-closed 不猜）";
        ++stats.decryptorAmbiguous;
        stats.rejects.push_back(std::move(rej));
        return std::nullopt;
    }
    const MaterialWindow& window = windows.front();

    const auto reject = [&](ca::usize bucket, std::string why) -> std::optional<Site> {
        // 拒绝记账入口（fail-closed：站点不动，原因进 trace）。
        Reject rej;
        rej.uri         = uri;
        rej.methodIndex = window.decryptorIndex;
        rej.why         = std::move(why);
        switch (bucket) {
        case 0: ++stats.keyBodyMismatch; break;
        case 1: ++stats.initBlocked; break;
        case 2: ++stats.initConflict; break;
        case 3: ++stats.writerShape; break;
        case 4: ++stats.writerUnguarded; break;
        case 5: ++stats.keyRefuted; break;
        default: ++stats.decryptorAmbiguous; break;
        }
        stats.rejects.push_back(std::move(rej));
        return std::nullopt;
    };

    // ---- promote 失败 = 写者普查不完整（缺方法的视图会漏 PUTSTATIC 站点，
    //      fail-closed：宁拒不猜）----
    if (!promoteOk) {
        return reject(3, "方法 promote 失败，写者普查不完整（fail-closed）");
    }

    // ---- materialKey() 体形 + 字段身份 ----
    const std::optional<MaterialKeyBody> body =
        matchMaterialKeyBody(s, views, window.materialKeyName);
    if (!body.has_value()) {
        return reject(0, "materialKey() 体非 createMaterialKeyMethod 逐字 9 条形（或字段不在场/非 private static）");
    }

    // ---- materialKey 调用点普查：全容器面（本类）恰 1 个（窗口内那个）----
    {
        ca::usize callers = 0;
        for (const auto& [mi, view] : views) {
            for (const ir::Insn* in : view.seq) {
                if (in->op != Op::INVOKESTATIC) continue;
                ir::ClassSession::MemberSem sem;
                if (memberOf(s, *in, ir::ClassSession::MemberSem::Kind::Method, sem)
                    && sem.owner == self && sem.name == window.materialKeyName
                    && sem.desc == "()I") {
                    ++callers;
                }
            }
        }
        if (callers != 1) {
            return reject(6, "materialKey() 调用点多于 1（生成面只在材料解密器体内调用一次）");
        }
    }

    // ---- clinit 定位 ----
    const ca::usize clinitIndex = findMethodSlot(unit, "<clinit>", "()V");
    const auto      clinitIt    = views.find(clinitIndex);
    if (clinitIndex == kNoSlot || clinitIt == views.end()) {
        return reject(1, "无 <clinit>（初值链无处在场）");
    }
    const MethodView& clinit = clinitIt->second;

    // ---- 写者普查（两字段独立）----
    FieldCensus keyCensus;
    FieldCensus poisonCensus;
    if (!censusField(s, views, clinitIndex, body->keyField, "J", keyCensus)
        || !censusField(s, views, clinitIndex, body->poisonField, "I", poisonCensus)) {
        return reject(3, "材料字段存在 clinit 外的非扰动窗口形写（字段被外写，fail-closed）");
    }
    for (const ca::usize mi : keyCensus.suspiciousMethods) {
        const auto it = views.find(mi);
        if (it == views.end() || !guardFingerprint(s, it->second)) {
            return reject(4, "keyField 扰动写所在方法缺守卫指纹（INVOKESTATIC ()I; DUP; IFEQ）");
        }
    }
    for (const ca::usize mi : poisonCensus.suspiciousMethods) {
        const auto it = views.find(mi);
        if (it == views.end() || !guardFingerprint(s, it->second)) {
            return reject(4, "poisonField 扰动写所在方法缺守卫指纹（INVOKESTATIC ()I; DUP; IFEQ）");
        }
    }
    const auto initOk = [](const FieldCensus& c) {
        return c.initAt.has_value() && c.initAt->first != kNoSlot;
    };
    if (!initOk(keyCensus) || !initOk(poisonCensus)) {
        return reject(2, "材料字段初值写缺席/多个（多写冲突或无 init 写）");
    }

    // ---- 初值求值 ----
    ca::i64     keyValue = 0;
    std::string blockedWhy;
    if (!evalKeyInit(s, views, clinit, keyCensus.initAt->second, keyValue, blockedWhy)) {
        return reject(1, "keyField " + blockedWhy);
    }
    ca::i32 poisonValue = 0;
    if (!evalPoisonInit(s, clinit, poisonCensus.initAt->second, poisonValue)) {
        return reject(1, "poisonField 初值走廊不可静态求值（fail-closed）");
    }

    // ---- 候选 materialKey = fold(keyField 初值) ^ poison 初值 ----
    const ca::i32 materialKey = materialFold(keyValue) ^ poisonValue;
    const ca::i32 effectiveKey = window.rawKey ^ materialKey;

    // ---- S1 互证：路由到本材料解密器的全部 indy 站点按候选 key 解密 ----
    // 路由（boar 实证配对面，grunt3 e41e852 ReferenceObfuscate 生成面）：
    // 名字族推导——解密器名 = BSM 名 B[2, len-1)（zA 实证：BSM
    // "6A1b27gwa9mAnYfh" ↔ 材料解密器 "1b27gwa9mAnYf"）∨ 体跟随（BSM 体首条
    // INVOKESTATIC <self> (String;)String 目标 == 材料解密器名）。
    std::set<ca::u32> routedBsms;
    for (ca::usize bi = 0; bi < bsms->size(); ++bi) {
        const auto& bsm = (*bsms)[bi];
        if (!s.backing().cp.valid(bsm.methodRef)) continue;
        const jbc::core::ConstantPool::Entry& mh = s.backing().cp.at(bsm.methodRef);
        if (mh.tag != CpTag::MethodHandle || !s.backing().cp.valid(mh.b)) continue;
        const jbc::core::ConstantPool::Entry& ref = s.backing().cp.at(mh.b);
        if ((ref.tag != CpTag::Methodref && ref.tag != CpTag::InterfaceMethodref)
            || !s.backing().cp.valid(ref.a) || !s.backing().cp.valid(ref.b)) {
            continue;
        }
        const jbc::core::ConstantPool::Entry& cls = s.backing().cp.at(ref.a);
        const jbc::core::ConstantPool::Entry& nat = s.backing().cp.at(ref.b);
        if (cls.tag != CpTag::Class || !s.backing().cp.valid(cls.a) || nat.tag != CpTag::NameAndType
            || !s.backing().cp.valid(nat.a) || !s.backing().cp.valid(nat.b)) {
            continue;
        }
        const std::string bsmOwner(std::string(s.backing().cp.utf8Of(cls.a)));
        if (bsmOwner != self) continue;
        const std::string bsmDesc(std::string(s.backing().cp.utf8Of(nat.b)));
        if (bsmDesc != knowledge::kGruntSimpleBsmDesc) continue;
        const std::string bsmName(std::string(s.backing().cp.utf8Of(nat.a)));
        bool routed = bsmName.size() >= 3
                   && bsmName.compare(2, bsmName.size() - 3, window.decryptorName) == 0;
        if (!routed) {
            // 体跟随：BSM 方法体内 INVOKESTATIC <self> (String;)String。
            const ca::usize bmi = findMethodSlot(unit, bsmName, bsmDesc);
            const auto      bit = views.find(bmi);
            if (bit != views.end()) {
                for (const ir::Insn* in : bit->second.seq) {
                    if (in->op != Op::INVOKESTATIC) continue;
                    ir::ClassSession::MemberSem sem;
                    if (memberOf(s, *in, ir::ClassSession::MemberSem::Kind::Method, sem)
                        && sem.owner == self
                        && sem.desc == knowledge::kGruntSimpleDecryptorDesc) {
                        routed = sem.name == window.decryptorName;
                        break;
                    }
                }
            }
        }
        if (routed) routedBsms.insert(static_cast<ca::u32>(bi));
    }

    ca::usize verified = 0;
    if (!routedBsms.empty()) {
        const jbc::core::ConstantPool& cp = s.backing().cp;
        for (const auto& [mi, view] : views) {
            for (ca::usize p = 0; p < view.seq.size(); ++p) {
                const ir::Insn* in = view.seq[p];
                if (in->op != Op::INVOKEDYNAMIC) continue;
                if (!cp.valid(in->cp)) continue;
                const jbc::core::ConstantPool::Entry& indy = cp.at(in->cp);
                if (indy.tag != CpTag::InvokeDynamic || indy.a >= bsms->size()
                    || routedBsms.count(indy.a) == 0) {
                    continue;
                }
                // indy desc（互证另一边）。
                if (!cp.valid(indy.b)) return reject(5, "indy NameAndType 索引无效");
                const jbc::core::ConstantPool::Entry& nat = cp.at(indy.b);
                if (nat.tag != CpTag::NameAndType || !cp.valid(nat.b)) {
                    return reject(5, "indy NameAndType 解析失败");
                }
                auto indyParsed = jbc::type::parseMethodDescriptor(cp.utf8Of(nat.b));
                if (indyParsed.is_err()) return reject(5, "indy desc 解析失败");
                const jbc::type::MethodDesc indyDesc = std::move(indyParsed).unwrap();
                // simple 4 参静态 args。
                const auto& args = (*bsms)[indy.a].args;
                if (args.size() != 4) return reject(5, "静态参数个数不符");
                std::string enc[3];
                ca::u32     isVirtual = 0;
                for (ca::usize k = 0; k < 3; ++k) {
                    if (!cp.valid(args[k])) return reject(5, "静态参数 CP 索引无效");
                    const jbc::core::ConstantPool::Entry& e = cp.at(args[k]);
                    if (e.tag != CpTag::String || !cp.valid(e.a)) return reject(5, "静态参数非 String");
                    enc[k] = std::string(cp.utf8Of(e.a));
                }
                {
                    if (!cp.valid(args[3])) return reject(5, "静态参数 CP 索引无效");
                    const jbc::core::ConstantPool::Entry& e = cp.at(args[3]);
                    if (e.tag != CpTag::Integer) return reject(5, "静态参数非 Integer");
                    isVirtual = static_cast<ca::u32>(e.ival);
                }
                if (isVirtual > 1) return reject(5, "isVirtual 越界（生成面只发 0/1）");
                std::string decOwner;
                std::string decName;
                std::string decDesc;
                if (!scan_tools::decryptXor(enc[0], effectiveKey, decOwner)
                    || !scan_tools::decryptXor(enc[1], effectiveKey, decName)
                    || !scan_tools::decryptXor(enc[2], effectiveKey, decDesc)) {
                    return reject(5, "候选 key 解密失败（MUTF-8 断链）");
                }
                if (decOwner.empty() || decName.empty() || decDesc.empty()) {
                    return reject(5, "候选 key 解密产物为空");
                }
                auto targetParsed = jbc::type::parseMethodDescriptor(decDesc);
                if (targetParsed.is_err()) return reject(5, "解密 desc 不是方法描述符");
                const jbc::type::MethodDesc targetDesc = std::move(targetParsed).unwrap();
                const auto sameType = [](const jbc::type::TypeDesc& a,
                                         const jbc::type::TypeDesc& b) {
                    if (a.kind != b.kind || a.array_dims != b.array_dims) return false;
                    if (a.kind == jbc::type::TypeDesc::Kind::Primitive) return a.primitive == b.primitive;
                    return a.class_name == b.class_name;
                };
                const auto sameParams = [&](const jbc::type::MethodDesc& a,
                                            const jbc::type::MethodDesc& b) {
                    if (a.params.size() != b.params.size()) return false;
                    for (ca::usize k = 0; k < a.params.size(); ++k) {
                        if (!sameType(a.params[k], b.params[k])) return false;
                    }
                    return true;
                };
                bool crossOk = false;
                if (isVirtual == 0) {
                    crossOk = sameParams(targetDesc, indyDesc)
                           && targetDesc.ret_void == indyDesc.ret_void
                           && (targetDesc.ret_void || sameType(targetDesc.ret, indyDesc.ret));
                } else {
                    crossOk = indyDesc.params.size() == targetDesc.params.size() + 1
                           && indyDesc.params[0].kind == jbc::type::TypeDesc::Kind::Class
                           && indyDesc.params[0].array_dims == 0
                           && indyDesc.params[0].class_name == "java/lang/Object";
                    if (crossOk) {
                        for (ca::usize k = 0; k < targetDesc.params.size(); ++k) {
                            if (!sameType(targetDesc.params[k], indyDesc.params[k + 1])) {
                                crossOk = false;
                                break;
                            }
                        }
                    }
                    if (crossOk) {
                        crossOk = targetDesc.ret_void == indyDesc.ret_void
                               && (targetDesc.ret_void || sameType(targetDesc.ret, indyDesc.ret));
                    }
                }
                if (!crossOk) {
                    return reject(5, "S1 互证失败：候选 key 解密产物与 indy desc 不符（站点 "
                                         + std::to_string(mi) + " pc " + std::to_string(in->srcPc) + "）");
                }
                ++verified;
            }
        }
    }

    Site site;
    site.uri                 = uri;
    site.decryptorMethodIndex = window.decryptorIndex;
    site.invokeSrcPc         = window.invokeSrcPc;
    site.decryptorRawKey     = window.rawKey;
    site.materialKey         = materialKey;
    site.sitesVerified       = verified;
    return site;
}

}   // namespace

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    static_cast<void>(input);
    ScanResult result;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        std::optional<Site> site = analyzeClass(*session, uri, result);
        if (site.has_value()) result.sites.push_back(std::move(*site));
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

namespace {

/// @brief 单站点改写：窗口复核（INVOKESTATIC 身份 + 邻接形）→ 原地换 LDC
///        <materialKey>（srcPc 保留）。@return false = 未动（failed 记账）。
bool rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu, const Site& site)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    const std::string              self(s.unit().symbols().str(s.unit().thisClass));

    ca::usize pos = kNoSlot;
    for (ca::usize p = 0; p < order.size(); ++p) {
        const ir::Insn* in = cu.dom.peek(order[p]);
        if (in != nullptr && in->op == Op::INVOKESTATIC && in->srcPc == site.invokeSrcPc) {
            pos = p;
            break;
        }
    }
    if (pos == kNoSlot) return false;

    // 形状复核：窗口身份 + 邻接形（scan 之后字节可能已被改写，不盲改）。
    // 窗口 = prev(常量 K) ← 本条 ← IXOR ← IXOR ← I2C，先钉跨度再取条。
    if (pos + 3 >= order.size() || pos == 0) return false;
    auto slotR = cu.dom.get(order[pos]);
    if (slotR.is_err()) return false;
    ir::Insn*     invoke  = slotR.unwrap();
    const ir::Insn original = *invoke;
    ir::ClassSession::MemberSem sem;
    if (invoke->op != Op::INVOKESTATIC || !s.memberOf(*invoke, sem)
        || sem.kind != ir::ClassSession::MemberSem::Kind::Method || sem.owner != self
        || sem.desc != "()I") {
        return false;
    }
    const ir::Insn* prev  = cu.dom.peek(order[pos - 1]);
    const ir::Insn* next1 = cu.dom.peek(order[pos + 1]);
    const ir::Insn* next2 = cu.dom.peek(order[pos + 2]);
    const ir::Insn* next3 = cu.dom.peek(order[pos + 3]);
    ca::i32         raw   = 0;
    if (prev == nullptr || next1 == nullptr || next2 == nullptr || next3 == nullptr
        || !const_args::decodeConstInt(s, *prev, raw) || raw != site.decryptorRawKey
        || next1->op != Op::IXOR || next2->op != Op::IXOR || next3->op != Op::I2C) {
        return false;
    }

    ir::Insn fresh;
    if (!rewrite_tools::makeConstLoad(s, jsim::SimValue::knownI32(site.materialKey), fresh)) {
        return false;
    }
    fresh.srcPc = original.srcPc;
    *invoke     = fresh;
    return true;
}

}   // namespace

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    rewrite_tools::FrameContext frames(container);

    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit = 0;
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
        for (const Site* site : sitesOf) methodIdxs.insert(site->decryptorMethodIndex);

        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit    = 0;
        ca::usize failed = 0;
        for (const Site* site : sitesOf) {
            if (safe.count(site->decryptorMethodIndex) == 0) {
                ++dirty.skippedByRoundTrip;
                continue;
            }
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, site->decryptorMethodIndex, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    if (!rewriteSite(s, cu, *site)) return false;
                    return true;
                });
            if (!ok) {
                ++failed;
                continue;
            }
            ++hit;
        }
        dirty.failed += failed;
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.hit;
        dirty.classes += 1;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

namespace {

/// @brief residual 计数：改写后容器里仍带材料解密器窗口形的类数（只跑窗口
///        形状面，不评初值链——residual 口径 = 形状仍在场）。会话开不了或
///        promote 失败的类按 residual 计（保守 fail-closed：无法证明窗口
///        已消失就计入残留）。
ca::usize countMaterialWindowClasses(const mjbase::model::ResourceContainer& container)
{
    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++residual;
            return;
        }
        std::unordered_map<ca::usize, MethodView> views;
        if (!promoteAllInto(*session, views)) {
            ++residual;
            return;
        }
        if (!findMaterialWindows(*session, views).empty()) ++residual;
    });
    return residual;
}

}   // namespace

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    pass_clock::Suspend                   suspend;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok       = intact.ok;
    result.failed   = intact.failed;
    result.residual = countMaterialWindowClasses(container);
    return result;
}

}   // namespace grunt::material_key_restore
