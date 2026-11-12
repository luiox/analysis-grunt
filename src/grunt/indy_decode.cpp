/// @file        indy_decode.cpp
/// @brief       残留 `indy_decode` 三段实现：key 提取（S2）→ mode/extra 表
///              分派（S3，knowledge 表）→ desc 互证（S1）→ 7 形态改写。
///
/// 形态、判据分级与插入式改写的锚保留/槽位面设计见头文件；骨架对齐
/// indy_fold / number_fold：scan 只读、rewrite 只写（mutateMethodWithFrames
/// 正统帧链写回）、proof 只判（类数不增 + 脏类可重解析 + residual 统计）。
///
/// ## 注入面
///
/// 文件系统副作用（JDK 接口普查）收敛在 scan 之前：CLI `--jdk` 传入 JDK
/// 根，adapter 在 probeEra 判为 Kasumi 时才扫描注入；残留本体零文件系统遍
/// 历。jdkInterfaces 为空 = 无任何旁证——virtual 站点目标不在容器时整站
/// fail-closed 拒绝（invokevirtual 误调接口方法运行时
/// IncompatibleClassChangeError，fail-closed 优先于覆盖率）。
///
/// ## SCAN
///
/// 两轮：第一轮全容器普查（各类 access 位收接口集、逐方法收解密器 key 与
/// BSM→解密器映射），第二轮逐方法逐 INVOKEDYNAMIC 判定；key 按内部类名归
/// 账。S2 双钉选面：BSM 8 参描述符指纹（scan_tools::bsmEntries 读
/// BootstrapMethods）+ 解密器体形（i2c 回溯两层 ixor 取常量异或，兼容单
/// 层）；指纹不钉的 indy 静默跳过——非本家调用点不记账。Era 门：probeEra
/// ≠ Kasumi 整站拒绝；mode/extra 表是 S3 换代面，Era 分支是它唯一合法的
/// 落点。key 链 bsmDecrypt[bsm] → dec → decryptors[dec] = key（BSM 体里第
/// 一条直调本类 (String;)String 的 invokestatic 即映射），任一环断 =
/// keyMissing。bsmArgs 恰 5 个 [Integer,String,String,String,Integer]，不
/// 符 = malformedArguments。
///
/// 解密是自写 MUTF-8 码元解码：孤立代理是合法形态（库的严格 UTF-8 解码会
/// 拒真），逐码元 XOR key 低 16 位后按 Java 规则重编码；owner 点分转内部
/// 名。ctor 的 encDesc 解出空 → init desc 由 indy 描述符参数段合成（描述
/// 符里 `)` 只在参数段尾出现一次，取最后一个）。S1 互证见头文件，不符 =
/// descMismatch 桶。
///
/// ## REWRITE
///
/// 按 uri 聚合，roundTripSafe 前置门（未改会话先往返，跑不完整方法整方法
/// 跳过）。纯原地三形态（static/getstatic/putstatic）换 op + setMember 换
/// CP 引用（先 intern 后动 DOM），srcPc 保留；换不动则 op 还原——失败不
/// 留半截指令。插入式四形态见头文件：indy 本体原地换指令，spill/reload
/// （或 NEW;DUP）序列插在 indy 之前，spill 槽从 cu->maxLocals 起排、成功
/// 后抬到新水位（超 u16 整站放弃）；插入失败 eraseBatch 回滚再报 failed。
/// 帧链失败粒度 = 类，7 形态分列计数只在类写回成功后归账。
///
/// ## 已知边界
///
/// 解密产物空 owner / 非 ctor 空 name → decodeFailed 拒绝，不猜。解密器形
/// 状变体（i2c 前多一层算术等）不认——换代面，Era 门兜底整站拒绝。
/// ConstantCallSite 假设：可变调用点形态不在面内。

#include "grunt/indy_decode.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "jbc/type/descriptor.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::indy_decode {
namespace {

namespace ir = jbc::ir;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::type::MethodDesc;
using jbc::type::TypeDesc;

constexpr ca::u16 kAccInterface = 0x0200;   ///< class access_flags 的 ACC_INTERFACE

/// rewriteSite 的成功产出：命中形态（DirtySet 形态计数由调用方在类写回
/// 成功后归账——类级放弃不许虚报站点）。
using FormHit = std::optional<knowledge::KasumiIndyForm>;

// ============================================================================
// ==== 描述符工具 ============================================================
// ============================================================================

/// @brief 结构等价比较：两条类型描述符（TypeDesc 视图指向不同输入串）。
bool sameType(const TypeDesc& a, const TypeDesc& b) noexcept
{
    if (a.kind != b.kind || a.array_dims != b.array_dims) return false;
    if (a.kind == TypeDesc::Kind::Primitive) return a.primitive == b.primitive;
    return a.class_name == b.class_name;
}

/// @brief 两条方法描述符参数段逐位等价。
bool sameParams(const MethodDesc& a, const MethodDesc& b) noexcept
{
    if (a.params.size() != b.params.size()) return false;
    for (ca::usize i = 0; i < a.params.size(); ++i) {
        if (!sameType(a.params[i], b.params[i])) return false;
    }
    return true;
}

/// @brief 引用类型判定（receiver/checkcast 面：类或数组）。
bool isRefType(const TypeDesc& t) noexcept
{
    return t.kind == TypeDesc::Kind::Class || t.array_dims > 0;
}

/// @brief 字段类型 → spill 用 load/store 变量指令（cat2 双槽由 slotWidth 承担）。
bool varOpFor(const TypeDesc& t, bool load, Op& out) noexcept
{
    if (isRefType(t)) {
        out = load ? Op::ALOAD : Op::ASTORE;
        return true;
    }
    switch (t.primitive) {
    case jbc::type::BaseType::Long:   out = load ? Op::LLOAD : Op::LSTORE; return true;
    case jbc::type::BaseType::Double: out = load ? Op::DLOAD : Op::DSTORE; return true;
    case jbc::type::BaseType::Float:  out = load ? Op::FLOAD : Op::FSTORE; return true;
    case jbc::type::BaseType::Byte:
    case jbc::type::BaseType::Char:
    case jbc::type::BaseType::Short:
    case jbc::type::BaseType::Int:
    case jbc::type::BaseType::Boolean:
        out = load ? Op::ILOAD : Op::ISTORE;
        return true;
    }
    return false;
}

/// @brief 方法描述符解析（空参/坏形返回 false，不抛）。
bool parseMethod(std::string_view desc, MethodDesc& out) noexcept
{
    auto r = jbc::type::parseMethodDescriptor(desc);
    if (r.is_err()) return false;
    out = std::move(r).unwrap();
    return true;
}

// ============================================================================
// ==== key 提取（S2：解密器形状；int 常量读取走 const_args）==================
// ============================================================================

/// @brief 从解密器方法体提取 XOR key。
///
/// Kasumi 双层形如 `charAt; LDC K1; LDC K2; IXOR; IXOR; i2c`（内层 IXOR 算
/// `K1^K2`，外层把 charAt 结果与之异或）；兼容单层 `LDC K; IXOR`。定位 `i2c`
/// （解密循环里把异或结果转回 char 的唯一位点）后按布局回溯：DOM 的 order
/// 是布局序，`p-1/p-2/…` 即 ASM previousReal 的等价面（无 LABEL/FRAME 夹层）。
std::optional<ca::i32> extractDecryptKey(const ir::ClassSession& s, ir::CodeUnit& cu)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    for (ca::usize p = 0; p < order.size(); ++p) {
        const ir::Insn* in = cu.dom.peek(order[p]);
        if (in == nullptr || in->op != Op::I2C) continue;
        if (p < 1) break;
        const ir::Insn* outer = cu.dom.peek(order[p - 1]);
        if (outer == nullptr || outer->op != Op::IXOR) continue;
        if (p < 2) break;
        const ir::Insn* inner = cu.dom.peek(order[p - 2]);
        if (inner == nullptr) break;
        // 双层：innerXor 前驱是 K2，再前驱是 K1，key = K1 ^ K2。
        if (inner->op == Op::IXOR) {
            if (p < 4) break;
            ca::i32 k2 = 0;
            if (!const_args::decodeConstInt(s, cu, order, p - 3, k2)) break;
            ca::i32 k1 = 0;
            if (!const_args::decodeConstInt(s, cu, order, p - 4, k1)) break;
            return k1 ^ k2;
        }
        // 单层：inner 位置直接是 int 常量。
        ca::i32 single = 0;
        if (const_args::decodeConstInt(s, cu, order, p - 2, single)) return single;
        break;
    }
    return std::nullopt;
}

// ============================================================================
// ==== 每类知识收集：解密器 key 与 BSM→解密器映射 ============================
// ============================================================================

/// @brief 一个类的 key 面：decryptors[dec-name]=key；bsmDecrypt[bsm-name]=dec-name。
struct ClassKeys
{
    std::unordered_map<std::string, ca::i32>     decryptors;
    std::unordered_map<std::string, std::string> bsmDecrypt;
};

/// @brief 收集一类的解密器 key 与 BSM→解密器映射（纯读，失败静默——缺表在
///        站点判定时走 keyMissing 记账）。
void collectClassKeys(ir::ClassSession& s, ClassKeys& out)
{
    const ir::ClassUnit& unit = s.unit();
    const std::string    self = std::string(unit.symbols().str(unit.thisClass));

    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        const std::string_view mdesc = unit.symbols().str(unit.methods[mi].desc);
        const std::string mname(unit.symbols().str(unit.methods[mi].name));

        if (mdesc == knowledge::kKasumiDecryptorDesc) {
            // 解密器候选：体形（charAt/双层 IXOR/i2c 或单层）钉事实才算数。
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            auto key = extractDecryptKey(s, *cu);
            if (key.has_value()) out.decryptors[mname] = *key;
            continue;
        }
        if (mdesc != knowledge::kKasumiBootstrapDesc) continue;
        // BSM 候选：体内第一条「直调本类 (String;)String 解密器」的
        // invokestatic 即映射。
        if (!ir::hasCode(s, mi)) continue;
        auto promoted = ir::promoteMethod(s, mi);
        if (promoted.is_err()) continue;
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;
        for (const ir::InsnId id : cu->dom.order()) {
            const ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr || in->op != Op::INVOKESTATIC) continue;
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*in, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Method) {
                continue;
            }
            if (sem.owner != self || sem.desc != knowledge::kKasumiDecryptorDesc) continue;
            out.bsmDecrypt[mname] = std::string(sem.name);
            break;
        }
    }
}

// ============================================================================
// ==== bsmArgs 读取与解密 ====================================================
// ============================================================================

/// @brief bootstrap 静态参数（5 个）读取：[Integer mode, String encOwner,
///        String encName, String encDesc, Integer extra]。
struct BsmArgs
{
    ca::u32     mode  = 0;
    ca::u32     extra = 0;
    std::string encOwner;
    std::string encName;
    std::string encDesc;
};

bool readBsmArgs(const jbc::core::ConstantPool& cp, const std::vector<ca::u32>& args,
                 BsmArgs& out)
{
    if (args.size() != knowledge::kKasumiBsmArgCount) return false;
    const auto intAt = [&](ca::u32 cpIndex, ca::u32& value) {
        if (!cp.valid(cpIndex)) return false;
        const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
        if (e.tag != CpTag::Integer) return false;
        value = static_cast<ca::u32>(e.ival);
        return true;
    };
    const auto strAt = [&](ca::u32 cpIndex, std::string& value) {
        if (!cp.valid(cpIndex)) return false;
        const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
        if (e.tag != CpTag::String || !cp.valid(e.a)) return false;
        value = std::string(cp.utf8Of(e.a));
        return true;
    };
    return intAt(args[0], out.mode) && strAt(args[1], out.encOwner)
           && strAt(args[2], out.encName) && strAt(args[3], out.encDesc)
           && intAt(args[4], out.extra);
}

/// @brief 解密 owner/name/desc 三段并过基本合法性（owner 非空；name 非空
///        除 ctor——new 不需要方法名）。owner 点分 → slash。
bool decryptTargets(const BsmArgs& args, ca::i32 key, bool ctor, std::string& owner,
                    std::string& name, std::string& desc, std::string& why)
{
    auto fail = [&why](const char* reason) {
        why = reason;
        return false;
    };
    std::string decOwner;
    std::string decName;
    std::string decDesc;
    if (!scan_tools::decryptXor(args.encOwner, key, decOwner)) return fail("owner 解密失败");
    if (!scan_tools::decryptXor(args.encName, key, decName)) return fail("name 解密失败");
    if (!scan_tools::decryptXor(args.encDesc, key, decDesc)) return fail("desc 解密失败");
    if (decOwner.empty()) return fail("owner 解密为空");
    if (decName.empty() && !ctor) return fail("name 解密为空");
    owner = jbc::type::binaryToInternal(decOwner).to_std_string();
    name  = decName.empty() ? std::string("<init>") : std::move(decName);
    desc  = std::move(decDesc);
    return true;
}

// ============================================================================
// ==== S1 交叉校验：indy desc 与解密结果互证 =================================
// ============================================================================

/// @brief 按 7 形态核对 indy 描述符与解密结果。ctor 的 init desc 在这里定案：
///        解密 desc 是合法方法描述符且参数段与 indy 一致时用之，否则按 indy
///        参数段（原始描述符串的参数段）合成 `(args)V`（Kasumi 对 ctor 解密
///        出空 desc，notes §2.5）。
bool crossCheck(knowledge::KasumiIndyForm form, const MethodDesc& indy,
                std::string_view indyDescRaw, std::string_view owner,
                std::string& targetDesc, std::string& why)
{
    auto fail = [&why](const char* reason) {
        why = reason;
        return false;
    };

    if (form == knowledge::KasumiIndyForm::Static || form == knowledge::KasumiIndyForm::Virtual)
    {
        MethodDesc target;
        if (!parseMethod(targetDesc, target)) return fail("目标 desc 不是方法描述符");
        if (form == knowledge::KasumiIndyForm::Static) {
            if (!sameParams(target, indy)) return fail("参数不符（static）");
        } else {
            // virtual：indy 首参是 receiver（引用型），其余与目标参数逐位等价。
            if (indy.params.size() != target.params.size() + 1) {
                return fail("receiver 槽缺失（virtual）");
            }
            if (!isRefType(indy.params[0])) return fail("receiver 不是引用类型");
            for (ca::usize i = 0; i < target.params.size(); ++i) {
                if (!sameType(target.params[i], indy.params[i + 1])) {
                    return fail("参数不符（virtual）");
                }
            }
        }
        if (target.ret_void != indy.ret_void
            || (!target.ret_void && !sameType(target.ret, indy.ret))) {
            return fail("返回类型不符");
        }
        return true;
    }

    if (form == knowledge::KasumiIndyForm::GetField
        || form == knowledge::KasumiIndyForm::PutField
        || form == knowledge::KasumiIndyForm::GetStatic
        || form == knowledge::KasumiIndyForm::PutStatic) {
        auto parsed = jbc::type::parseFieldType(targetDesc);
        if (parsed.is_err()) return fail("目标 desc 不是字段描述符");
        const TypeDesc field = std::move(parsed).unwrap();
        const bool isStatic = form == knowledge::KasumiIndyForm::GetStatic
                              || form == knowledge::KasumiIndyForm::PutStatic;
        const bool isGetter = form == knowledge::KasumiIndyForm::GetField
                              || form == knowledge::KasumiIndyForm::GetStatic;
        if (isStatic) {
            // 静态字段：indy desc 无 receiver——getter ()T / setter (T)V。
            if (isGetter) {
                if (!indy.params.empty()) return fail("static getter 带参数");
                if (indy.ret_void || !sameType(indy.ret, field)) return fail("getter 类型不符");
            } else {
                if (indy.params.size() != 1 || !sameType(indy.params[0], field)) {
                    return fail("static setter 实参类型不符");
                }
                if (!indy.ret_void) return fail("setter 返回类型不符");
            }
        } else {
            // 实例字段：首参 Object(receiver)——getter (Object)T /
            // setter (Object,T)V。
            if (isGetter) {
                if (indy.params.size() != 1 || !isRefType(indy.params[0])) {
                    return fail("getter receiver 缺失");
                }
                if (indy.ret_void || !sameType(indy.ret, field)) return fail("getter 类型不符");
            } else {
                if (indy.params.size() != 2 || !isRefType(indy.params[0])
                    || !sameType(indy.params[1], field)) {
                    return fail("setter 实参类型不符");
                }
                if (!indy.ret_void) return fail("setter 返回类型不符");
            }
        }
        return true;
    }

    // Constructor：indy 返回 Lowner;；init desc 参数段必须与 indy 参数段一致
    // （换指令不改栈）。
    if (indy.ret_void || indy.ret.kind != TypeDesc::Kind::Class || indy.ret.array_dims != 0
        || indy.ret.class_name != owner) {
        return fail("返回类型不是 Lowner;（ctor）");
    }
    MethodDesc init;
    if (parseMethod(targetDesc, init)) {
        if (!sameParams(init, indy)) return fail("ctor 参数不符");
        return true;   // targetDesc 已是可用 init desc
    }
    // 解密 desc 为空/非方法形（Kasumi 常态）：按 indy 原始描述符的参数段合成
    // （JVMS 方法描述符里 ')' 只在参数段结束出现一次，取到最后 ')' 即参数尾）。
    const ca::usize close = indyDescRaw.rfind(')');
    if (close == std::string_view::npos) return fail("indy desc 无参数段（ctor）");
    targetDesc = std::string(indyDescRaw.substr(0, close + 1)) + "V";
    return true;
}

// ============================================================================
// ==== 单站点解密判定（scan 细找）============================================
// ============================================================================

enum class Verdict
{
    Skip,    ///< 非 Kasumi indy / CP 不可归属：静默跳过（不进 rejects）
    Accept,  ///< 站点成立
    Reject,  ///< fail-closed 拒绝（进 rejects，bucket 已记账）
};

/// @brief 一个 indy 指令位置 → Site / Reject / Skip。
Verdict judgeSite(ir::CodeUnit& cu, const std::string& uri, ca::usize methodIndex,
                  ca::u32 srcPc, const ir::Insn& in,
                  const std::vector<jbc::core::BootstrapMethodsAttr::Entry>& bsms,
                  const jbc::core::ConstantPool& cp,
                  const std::unordered_map<std::string, ClassKeys>& keysByOwner,
                  const std::unordered_map<std::string, bool>& jarIsInterface,
                  const ScanInput& input, Site& outSite, Reject& outReject, ScanResult& stats)
{
    const auto reject = [&](std::string why, ca::usize bucket) {
        outReject.uri         = uri;
        outReject.methodIndex = methodIndex;
        outReject.indySrcPc   = srcPc;
        outReject.why         = std::move(why);
        switch (bucket) {
        case 0: ++stats.eraBlocked; break;
        case 1: ++stats.keyMissing; break;
        case 2: ++stats.malformedArguments; break;
        case 3: ++stats.decodeFailed; break;
        case 4: ++stats.descMismatch; break;
        default: ++stats.interfaceUnattested; break;
        }
        return Verdict::Reject;
    };

    // CP 不可归属（坏 classfile / 非 indy 语义）：静默跳过，不算 Kasumi 账。
    if (!cp.valid(in.cp)) return Verdict::Skip;
    const jbc::core::ConstantPool::Entry& indy = cp.at(in.cp);
    if (indy.tag != CpTag::InvokeDynamic) return Verdict::Skip;
    if (indy.a >= bsms.size()) return Verdict::Skip;

    // ---- S2 第一钉：8 参 BSM 指纹 ----
    const jbc::core::BootstrapMethodsAttr::Entry& bsm = bsms[indy.a];
    std::string bsmOwner;
    std::string bsmName;
    std::string bsmDesc;
    if (cp.valid(bsm.methodRef)) {
        const jbc::core::ConstantPool::Entry& mh = cp.at(bsm.methodRef);
        if (mh.tag == CpTag::MethodHandle && cp.valid(mh.b)) {
            const jbc::core::ConstantPool::Entry& ref = cp.at(mh.b);
            if ((ref.tag == CpTag::Methodref || ref.tag == CpTag::InterfaceMethodref)
                && cp.valid(ref.a) && cp.valid(ref.b)) {
                // Methodref 两层：a=Class（其 a 才是 Utf8 内部名），b=NameAndType。
                const jbc::core::ConstantPool::Entry& cls = cp.at(ref.a);
                const jbc::core::ConstantPool::Entry& nat = cp.at(ref.b);
                if (cls.tag == CpTag::Class && cp.valid(cls.a) && nat.tag == CpTag::NameAndType
                    && cp.valid(nat.a) && cp.valid(nat.b)) {
                    bsmOwner = std::string(cp.utf8Of(cls.a));
                    bsmName  = std::string(cp.utf8Of(nat.a));
                    bsmDesc  = std::string(cp.utf8Of(nat.b));
                }
            }
        }
    }
    if (bsmOwner.empty() || bsmDesc != knowledge::kKasumiBootstrapDesc) {
        return Verdict::Skip;   // 普通 indy（LambdaMetafactory 等 / 非 Kasumi BSM）
    }
    ++stats.totalIndy;

    // ---- S3 的 Era 门：mode/extra 数值表只在 Kasumi 判定下可用 ----
    if (input.era != knowledge::Era::Kasumi) {
        return reject("Era 未判定：mode/extra 表不可用（fail-closed）", 0);
    }

    // ---- S2 第二钉：key 链（bsmOwner → bsmName → dec-name → key）----
    ca::i32 key = 0;
    {
        const auto kit = keysByOwner.find(bsmOwner);
        if (kit == keysByOwner.end()) return reject("BSM owner 无解密器表", 1);
        const auto nit = kit->second.bsmDecrypt.find(bsmName);
        if (nit == kit->second.bsmDecrypt.end()) return reject("BSM→解密器映射缺失", 1);
        const auto dit = kit->second.decryptors.find(nit->second);
        if (dit == kit->second.decryptors.end()) return reject("解密器 key 缺失", 1);
        key = dit->second;
    }

    // ---- bsmArgs 读取 + 解密 + 7 形态表 ----
    BsmArgs args;
    if (!readBsmArgs(cp, bsm.args, args)) {
        return reject("bsmArgs 个数/种类不符（需 [Integer,String,String,String,Integer]）", 2);
    }
    const bool ctor = args.mode == knowledge::kKasumiModeCtor;

    knowledge::KasumiIndyForm form = knowledge::KasumiIndyForm::Static;
    if (!knowledge::kasumiIndyForm(args.mode, args.extra, form)) {
        return reject("mode/extra 组合在 7 形态表外", 3);
    }

    Site site;
    site.uri         = uri;
    site.methodIndex = methodIndex;
    site.indySrcPc   = srcPc;
    site.form        = form;

    std::string why;
    if (!decryptTargets(args, key, ctor, site.targetOwner, site.targetName, site.targetDesc,
                        why)) {
        return reject(std::move(why), 3);
    }

    // ---- S1：indy desc 与解密结果互证 ----
    if (!cp.valid(indy.b)) return reject("indy NameAndType 索引无效", 4);
    const jbc::core::ConstantPool::Entry& nat = cp.at(indy.b);
    if (nat.tag != CpTag::NameAndType || !cp.valid(nat.b)) {
        return reject("indy NameAndType 解析失败", 4);
    }
    MethodDesc indyDesc;
    const std::string indyDescRaw(cp.utf8Of(nat.b));
    if (!parseMethod(indyDescRaw, indyDesc)) return reject("indy desc 解析失败", 4);
    if (!crossCheck(form, indyDesc, indyDescRaw, site.targetOwner, site.targetDesc, why)) {
        return reject(std::move(why), 4);
    }

    // ---- 接口旁证（virtual 落点分派）：容器普查优先（权威）。JDK 侧是
    //      穷举普查（scanJdkInterfaces 收全集内全部接口），故**成员与否双向
    //      作证**：在集 = 接口（invokeinterface），不在集 = 类（invokevirtual
    //      ——ArrayList 这类 JDK 类目标靠缺席证据落对）。集合为空 = 没有任何
    //      旁证，fail-closed 拒绝（invokevirtual 误调接口方法在运行时
    //      IncompatibleClassChangeError）。
    if (form == knowledge::KasumiIndyForm::Virtual) {
        const auto jit = jarIsInterface.find(site.targetOwner);
        if (jit != jarIsInterface.end()) {
            site.ownerIsInterface = jit->second;
        } else if (!input.jdkInterfaces.empty()) {
            site.ownerIsInterface = input.jdkInterfaces.count(site.targetOwner) != 0;
        } else {
            return reject("JDK 接口集未注入，virtual 落点无旁证（fail-closed）：" + site.targetOwner,
                          5);
        }
    }

    outSite = std::move(site);
    return Verdict::Accept;
}

// ============================================================================
// ==== 改写原语 ==============================================================
// ============================================================================

/// @brief 一条合成指令（srcPc 由 CodeDom 硬清 kNoPc——锚保留设计见头文件）。
ir::Insn synthInsn(Op op, ca::u32 slot)
{
    ir::Insn in;
    in.op = op;
    in.a  = static_cast<ca::i32>(slot);
    return in;
}

/// @brief 单站点改写。@return 命中形态；nullopt = 未动（failed 记账）。
///
/// 重定位 + 复核（scan 之后字节可能已被同方法前站改写，不盲改）；插入式站点
/// 先插 spill/reload（或 NEW;DUP）再原地换指令——indy 本体保 srcPc。
FormHit rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu,
                    const std::vector<jbc::core::BootstrapMethodsAttr::Entry>& bsms,
                    const Site& site, ca::usize& failed)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    ir::InsnId                     indyId{};

    for (const ir::InsnId id : order) {
        const ir::Insn* in = cu.dom.peek(id);
        if (in != nullptr && in->op == Op::INVOKEDYNAMIC && in->srcPc == site.indySrcPc) {
            indyId = id;
            break;
        }
    }
    if (indyId == ir::kNoInsn) {
        ++failed;
        return std::nullopt;
    }
    auto slotR = cu.dom.get(indyId);
    if (slotR.is_err()) {
        ++failed;
        return std::nullopt;
    }
    ir::Insn* in = slotR.unwrap();
    if (in->op != Op::INVOKEDYNAMIC) {
        ++failed;
        return std::nullopt;
    }
    // 站点复核：BSM 指纹必须还是 Kasumi（防漂移）。
    const jbc::core::ConstantPool& cp = s.backing().cp;
    if (!cp.valid(in->cp) || cp.at(in->cp).tag != CpTag::InvokeDynamic
        || cp.at(in->cp).a >= bsms.size()) {
        ++failed;
        return std::nullopt;
    }
    const jbc::core::BootstrapMethodsAttr::Entry& bsm = bsms[cp.at(in->cp).a];
    if (!cp.valid(bsm.methodRef) || cp.at(bsm.methodRef).tag != CpTag::MethodHandle) {
        ++failed;
        return std::nullopt;
    }

    // ---- 原地换指令（失败把 op 还原：不留半截指令）----
    const auto swapInvoke = [&](Op op, bool interfaceKind) {
        in->op = op;
        const bool ok = s.setMember(
            *in, interfaceKind ? ir::ClassSession::MemberSem::Kind::InterfaceMethod
                               : ir::ClassSession::MemberSem::Kind::Method,
            site.targetOwner, site.targetName, site.targetDesc);
        if (!ok) in->op = Op::INVOKEDYNAMIC;
        return ok;
    };
    const auto swapField = [&](Op op) {
        in->op = op;
        const bool ok = s.setMember(*in, ir::ClassSession::MemberSem::Kind::Field,
                                    site.targetOwner, site.targetName, site.targetDesc);
        if (!ok) in->op = Op::INVOKEDYNAMIC;
        return ok;
    };

    // ---- 纯原地三态：static / getstatic / putstatic ----
    switch (site.form) {
    case knowledge::KasumiIndyForm::Static:
        if (!swapInvoke(Op::INVOKESTATIC, false)) {
            ++failed;
            return std::nullopt;
        }
        return site.form;
    case knowledge::KasumiIndyForm::GetStatic:
        if (!swapField(Op::GETSTATIC)) {
            ++failed;
            return std::nullopt;
        }
        return site.form;
    case knowledge::KasumiIndyForm::PutStatic:
        if (!swapField(Op::PUTSTATIC)) {
            ++failed;
            return std::nullopt;
        }
        return site.form;
    default: break;
    }

    // ---- 插入式四态：spill/reload（或 NEW;DUP）计划 ----
    //
    // 槽位面：从 cu->maxLocals 起排（receiver/value + 实参按 slotWidth 累计，
    // cat2 双槽）；超 u16 整站放弃。布局序：
    //   virtual:   [实参逆序 spill] [ASTORE recv] [ALOAD recv] [CHECKCAST]
    //              [实参顺序 reload]
    //   putfield:  [值 spill] [ASTORE recv] [ALOAD recv] [CHECKCAST] [值 reload]
    //   getfield:  [CHECKCAST]
    //   ctor:      [实参逆序 spill] [NEW] [DUP] [实参顺序 reload]
    // 插入序 = 布局序（insertBefore(anchor,…) 逐条落在 anchor 之前）。
    std::vector<ir::Insn> synth;
    ca::u32               slotBase = cu.maxLocals;
    ca::u32               used     = 0;
    bool                  planned  = true;

    if (site.form == knowledge::KasumiIndyForm::GetField) {
        // getter：栈顶就是 receiver，checkcast 后直接 getfield（无需 spill）。
        ir::Insn cast;
        cast.op = Op::CHECKCAST;
        synth.push_back(cast);
    } else if (site.form == knowledge::KasumiIndyForm::PutField) {
        auto parsed = jbc::type::parseFieldType(site.targetDesc);
        if (parsed.is_err()) {
            planned = false;
        } else {
            const TypeDesc field = std::move(parsed).unwrap();
            Op store = Op::NOP;
            Op load  = Op::NOP;
            if (!varOpFor(field, false, store) || !varOpFor(field, true, load)) {
                planned = false;
            } else {
                const ca::u32 valWidth = jbc::type::slotWidth(field);
                const ca::u32 recvSlot = slotBase + valWidth;
                used = valWidth + 1;
                synth.push_back(synthInsn(store, slotBase));   // 值 spill
                synth.push_back(synthInsn(Op::ASTORE, recvSlot));
                synth.push_back(synthInsn(Op::ALOAD, recvSlot));
                {
                    ir::Insn cast;
                    cast.op = Op::CHECKCAST;
                    synth.push_back(cast);
                }
                synth.push_back(synthInsn(load, slotBase));   // 值 reload
            }
        }
    } else if (site.form == knowledge::KasumiIndyForm::Virtual) {
        MethodDesc target;
        if (!parseMethod(site.targetDesc, target)) {
            planned = false;
        } else {
            // 实参逆序 spill（栈顶是末实参），再 spill receiver。
            std::vector<std::pair<Op, ca::u32>> argSpill;
            ca::u32                             next = slotBase + 1;   // 槽 0 留给 receiver
            for (const TypeDesc& p : target.params) {
                Op store = Op::NOP;
                if (!varOpFor(p, false, store)) {
                    planned = false;
                    break;
                }
                argSpill.push_back({store, next});
                next += jbc::type::slotWidth(p);
            }
            if (planned) {
                used = static_cast<ca::u32>(next - slotBase);
                for (ca::usize i = argSpill.size(); i-- > 0;) {
                    synth.push_back(synthInsn(argSpill[i].first, argSpill[i].second));
                }
                synth.push_back(synthInsn(Op::ASTORE, slotBase));
                synth.push_back(synthInsn(Op::ALOAD, slotBase));
                {
                    ir::Insn cast;
                    cast.op = Op::CHECKCAST;
                    synth.push_back(cast);
                }
                ca::u32 cursor = slotBase + 1;
                ca::usize pi   = 0;
                for (const TypeDesc& p : target.params) {
                    Op load = Op::NOP;
                    if (!varOpFor(p, true, load)) {
                        planned = false;
                        break;
                    }
                    synth.push_back(synthInsn(load, cursor));
                    cursor += jbc::type::slotWidth(p);
                    ++pi;
                }
            }
        }
    } else if (site.form == knowledge::KasumiIndyForm::Constructor) {
        MethodDesc init;
        if (!parseMethod(site.targetDesc, init)) {
            planned = false;
        } else {
            // JVM 的 uninitialized 类型追踪要求 NEW;DUP 紧邻参数序列：先逆序
            // spill 参数，再 NEW;DUP，再顺序 reload。
            std::vector<std::pair<Op, ca::u32>> spill;
            ca::u32                             next = slotBase;
            for (const TypeDesc& p : init.params) {
                Op store = Op::NOP;
                if (!varOpFor(p, false, store)) {
                    planned = false;
                    break;
                }
                spill.push_back({store, next});
                next += jbc::type::slotWidth(p);
            }
            if (planned) {
                used = static_cast<ca::u32>(next - slotBase);
                for (ca::usize i = spill.size(); i-- > 0;) {
                    synth.push_back(synthInsn(spill[i].first, spill[i].second));
                }
                {
                    ir::Insn nw;
                    nw.op = Op::NEW;
                    synth.push_back(nw);
                    ir::Insn dup;
                    dup.op = Op::DUP;
                    synth.push_back(dup);
                }
                ca::u32 cursor = slotBase;
                for (const TypeDesc& p : init.params) {
                    Op load = Op::NOP;
                    if (!varOpFor(p, true, load)) {
                        planned = false;
                        break;
                    }
                    synth.push_back(synthInsn(load, cursor));
                    cursor += jbc::type::slotWidth(p);
                }
            }
        }
    }
    if (!planned) {
        ++failed;
        return std::nullopt;
    }
    if (slotBase > 0xFFFF || used > 0xFFFF - slotBase) {
        ++failed;
        return std::nullopt;   // u16 maxLocals 域超界
    }

    // ---- CP 预 intern：失败面在改 DOM 之前收口（checkcast/NEW 类名）----
    for (ir::Insn& spec : synth) {
        if (spec.op == Op::CHECKCAST || spec.op == Op::NEW) {
            spec.cp = s.internClass(site.targetOwner);
            if (spec.cp == 0) {
                ++failed;
                return std::nullopt;
            }
        }
    }

    // ---- 插入 + 原地换指令 ----
    std::vector<ir::InsnId> inserted;
    for (const ir::Insn& spec : synth) {
        auto r = cu.dom.insertBefore(indyId, spec);
        if (r.is_err()) {
            if (!inserted.empty()) (void)cu.dom.eraseBatch(inserted);
            ++failed;
            return std::nullopt;
        }
        inserted.push_back(std::move(r).unwrap());
    }

    bool swapped = false;
    switch (site.form) {
    case knowledge::KasumiIndyForm::GetField:  swapped = swapField(Op::GETFIELD); break;
    case knowledge::KasumiIndyForm::PutField:  swapped = swapField(Op::PUTFIELD); break;
    case knowledge::KasumiIndyForm::Virtual:
        swapped = swapInvoke(site.ownerIsInterface ? Op::INVOKEINTERFACE : Op::INVOKEVIRTUAL,
                             site.ownerIsInterface);
        break;
    case knowledge::KasumiIndyForm::Constructor:
        swapped = swapInvoke(Op::INVOKESPECIAL, false);
        break;
    default: break;
    }
    if (!swapped) {
        // 换指令失败：拔回插入块（合成指令无标签/引用，kRejectIfUsed 安全）。
        if (!inserted.empty()) (void)cu.dom.eraseBatch(inserted);
        ++failed;
        return std::nullopt;
    }
    cu.maxLocals = static_cast<ca::u16>(slotBase + used);   // 槽位面抬到新水位
    return site.form;
}

/// @brief 单方法 REWRITE 驱动。@return 命中形态列表（调用方归账）。
///        cu 由 mutateMethodWithFrames 提升好：promote 只发生一次。
std::vector<knowledge::KasumiIndyForm> rewriteMethod(ir::ClassSession& s, ir::CodeUnit& cu,
                                                     const std::vector<const Site*>& sites,
                                                     const std::vector<
                                                         jbc::core::BootstrapMethodsAttr::Entry>&
                                                         bsms,
                                                     ca::usize& failed)
{
    std::vector<knowledge::KasumiIndyForm> hits;
    for (const Site* site : sites) {
        auto hit = rewriteSite(s, cu, bsms, *site, failed);
        if (hit.has_value()) hits.push_back(*hit);
    }
    return hits;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    ScanResult result;

    // 第一遍：接口普查（全容器——indy 目标可能是无 indy 形状的接口）+ 每类
    // key/BSM 映射收集。
    std::unordered_map<std::string, bool>      jarIsInterface;
    std::unordered_map<std::string, ClassKeys> keysByOwner;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        const ir::ClassUnit& unit = session->unit();
        const std::string    self = std::string(unit.symbols().str(unit.thisClass));
        jarIsInterface[self]      = (unit.access & kAccInterface) != 0;
        ClassKeys& keys           = keysByOwner[self];
        collectClassKeys(*session, keys);
    });

    // 第二遍：站点判定（粗找 = INVOKEDYNAMIC 全量起锚；细找 = judgeSite 的
    // S2/S3/S1 分级）。
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto         session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        const auto* bsms = scan_tools::bsmEntries(*session);
        if (bsms == nullptr) return;   // 无 BootstrapMethods：无站点

        ir::ClassSession&    s    = *session;
        const jbc::core::ConstantPool& cp = s.backing().cp;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            const std::vector<ir::InsnId>& order = cu->dom.order();
            for (ca::usize pc = 0; pc < order.size(); ++pc) {
                const ir::Insn* in = cu->dom.peek(order[pc]);
                if (in == nullptr || in->op != Op::INVOKEDYNAMIC) continue;
                Site   site;
                Reject rej;
                const Verdict verdict = judgeSite(*cu, uri, mi, in->srcPc, *in, *bsms, cp,
                                                  keysByOwner, jarIsInterface, input, site,
                                                  rej, result);
                if (verdict == Verdict::Accept) {
                    result.sites.push_back(std::move(site));
                } else if (verdict == Verdict::Reject) {
                    result.rejects.push_back(std::move(rej));
                }
            }
            // promoted 在此释放：scan 不写回，方法体保持原样。
        }
    });

    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    // 站点按 uri 聚类（对齐 number_fold：万类大罐上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string              uri;
        std::vector<ca::u8>      bytes;
        ca::usize                sites = 0;
        std::vector<knowledge::KasumiIndyForm> forms;
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
        const auto*       bsms = scan_tools::bsmEntries(s);

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        // 前置门：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit = 0;
        std::vector<knowledge::KasumiIndyForm> forms;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize failed = 0;
            std::vector<knowledge::KasumiIndyForm> methodForms;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    methodForms = rewriteMethod(s, cu, sitesOfMethod, *bsms, failed);
                    return !methodForms.empty();
                });
            if (!ok) {
                // 帧链/编码失败：该方法整方法放弃（session 内半改写态不外泄）。
                dirty.failed += failed + methodForms.size();
                continue;
            }
            hit += methodForms.size();
            dirty.failed += failed;
            forms.insert(forms.end(), methodForms.begin(), methodForms.end());
        }
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit,
                                       std::move(forms)});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.sites;
        dirty.classes += 1;
        for (const knowledge::KasumiIndyForm form : item.forms) {
            switch (form) {
            case knowledge::KasumiIndyForm::Static:      ++dirty.rewrittenStatic; break;
            case knowledge::KasumiIndyForm::Virtual:     ++dirty.rewrittenVirtual; break;
            case knowledge::KasumiIndyForm::GetField:    ++dirty.rewrittenGetField; break;
            case knowledge::KasumiIndyForm::PutField:    ++dirty.rewrittenPutField; break;
            case knowledge::KasumiIndyForm::GetStatic:   ++dirty.rewrittenGetStatic; break;
            case knowledge::KasumiIndyForm::PutStatic:   ++dirty.rewrittenPutStatic; break;
            case knowledge::KasumiIndyForm::Constructor: ++dirty.rewrittenNew; break;
            }
        }
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

namespace {

/// @brief 容器里仍带 Kasumi 8 参指纹的 indy 调用点数（residual 口径）。
ca::usize countKasumiIndyResidual(const mjbase::model::ResourceContainer& container)
{
    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        const auto* bsms = scan_tools::bsmEntries(*session);
        if (bsms == nullptr) return;

        // 指纹条目下标集（proof 只判不猜：desc 全等才算 Kasumi BSM）。
        std::set<ca::usize> kasumiBsm;
        const jbc::core::ConstantPool& cp = session->backing().cp;
        for (ca::usize i = 0; i < bsms->size(); ++i) {
            const jbc::core::BootstrapMethodsAttr::Entry& bsm = (*bsms)[i];
            if (!cp.valid(bsm.methodRef)) continue;
            const jbc::core::ConstantPool::Entry& mh = cp.at(bsm.methodRef);
            if (mh.tag != CpTag::MethodHandle || !cp.valid(mh.b)) continue;
            const jbc::core::ConstantPool::Entry& ref = cp.at(mh.b);
            if ((ref.tag != CpTag::Methodref && ref.tag != CpTag::InterfaceMethodref)
                || !cp.valid(ref.b)) {
                continue;
            }
            const jbc::core::ConstantPool::Entry& nat = cp.at(ref.b);
            if (nat.tag == CpTag::NameAndType && cp.valid(nat.b)
                && cp.utf8Of(nat.b) == knowledge::kKasumiBootstrapDesc) {
                kasumiBsm.insert(i);
            }
        }
        if (kasumiBsm.empty()) return;

        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            for (const ir::InsnId id : cu->dom.order()) {
                const ir::Insn* in = cu->dom.peek(id);
                if (in == nullptr || in->op != Op::INVOKEDYNAMIC || !cp.valid(in->cp)) continue;
                const jbc::core::ConstantPool::Entry& indy = cp.at(in->cp);
                if (indy.tag == CpTag::InvokeDynamic && kasumiBsm.count(indy.a) != 0) {
                    ++residual;
                }
            }
        }
    });
    return residual;
}

}   // namespace

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    // proof 是效力门（不变量复核），不许被 pass 级时间片截短——挂起期间
    // forEachClass 全量放行（residual 统计同样要看到全量类）。
    pass_clock::Suspend    suspend;
    ProofResult            result;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    result.ok     = intact.ok;
    result.failed = intact.failed;
    result.residual = countKasumiIndyResidual(container);
    return result;
}

}   // namespace grunt::indy_decode
