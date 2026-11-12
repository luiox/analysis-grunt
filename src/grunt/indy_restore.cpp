/// @file        indy_restore.cpp
/// @brief       残留 `indy_restore` 三段实现：键链三路（S2）→ 参数解密
///              （纯算术直解）→ S1 互证 → heavy Era 门 → 三态原地改写。
///
/// 形态、判据分级、键链三路与材料通道边界见头文件；骨架对齐 indy_decode /
/// string_pool_fold：scan 只读、rewrite 只写（mutateMethodWithFrames 正统
/// 帧链写回）、proof 只判（类数不增 + 脏类可重解析 + residual 统计）。
///
/// ## 扫描管线（每类一遍，纯读）
///
/// collectClassKeys 先建类内键表：(String;)String 方法 promote 后
/// scanDecryptKey 提窗口（i2c 回溯两层，布局序 = DOM order，与 indy_decode
/// 同源）——单层 `LDC K; IXOR` 记 Plain；`LDC K; INVOKESTATIC <self> ()I;
/// IXOR; IXOR` 记 Material（记原始 K）；`LDC A; LDC B; IXOR; IXOR`（reobf
/// 折叠键）记 Plain(A^B)。无方法体 / promote 失败 / 无窗口进
/// deadStringMethods（族推导 c2 的家族证据）。BSM 方法体在场才进表：
/// simple 取体首条直调本类 (String;)String 的解密器；heavy 取
/// (String;I)String 紧前的 int 常量 generalKey。judgeSite 粗找 =
/// INVOKEDYNAMIC 全量起锚，细找按 S2 desc 指纹 → 键链 → S1 解密 + desc 互
/// 证 → S3 heavy Era 门 + magic 自证 → 接口旁证（容器普查优先 / `--jdk`
/// 注入的 JDK 接口集双向作证 / 两无 fail-closed）。JDK 接口集由 adapter 在
/// Era 非 Unknown 时注入（scanJdkInterfaces 与 indy_decode 单源），残留本
/// 体零文件系统副作用。
///
/// ## 族推导只认 c1/c2 两条边
///
/// c1：解密器名 D 满足 len(D) = len(BSM名) - 3 且 BSM 名 substr(2) 前缀 ==
/// D。c2（plain BSM 壳被 NativePipeline 摘成 ACC_NATIVE 空壳时）：材料
/// BSM 名 N 与解密器名 M 满足 len(M) = len(N)+1 且 M 去尾 == N 去头，且
/// deadStringMethods 里有同名死方法佐证。c2 的 key 正确性源锚是生成面两分
/// 支烧同一 decryptKey 常量（材料通道 LDC 是异或前的原始 key，plain 站点
/// 可直接用）。**不认** plainDecryptName（len-4）这条边：它由路由 1 的同
/// 名面覆盖，混进族推导会把材料 BSM 的站点误接到 plain 解密器上——解出垃
/// 圾死在 S1 虽然不会误改，但拒绝记账口径被污染。
///
/// ## 解密与编码
///
/// 密文是逐 UTF-16 码元自逆 XOR，纯算术 C++ 转写直解（无 jsim 面）。
/// MUTF-8 直解不过严格 UTF-8：密文里合法存在 Java 孤立代理码元（classfile
/// 以 3 字节 CESU-8 形承载），严格解码器会拒真；与 indy_decode 的同名面同
/// 形独立实现。heavy 的 `data1 % 59557` 自证按 parseIntStrict 后取模复算
/// （data1 恒非负，JVM IREM 与 C++ % 在同号域语义一致）。
///
/// ## REWRITE
///
/// 站点按 uri 聚类，roundTripSafe 前置门；rewriteSite 按 indySrcPc 重定位
/// 并复核仍是 INVOKEDYNAMIC，然后纯原地换 op + setMember——不插指令（v2/
/// v3 无 spill 面，virtual 的 receiver 槽就是 indy 首参）。换指令失败把 op
/// 还原，不留半截。形态计数在类写回成功后归账（类级放弃不许虚报站点，
/// 对齐 indy_decode 口径）。
///
/// ## 边界与不做
///
/// 材料通道不猜（见头文件）。v2 专面不建：heavy 的 @GruntMetadata 注解名
/// 可配置、heavy 关闭时注解类不生成，不能当版本判据——probeEra 的 Grunt2
/// 判据因此在 era.hpp 明文挂缺（缺口 #15 口径），v2 现场的 heavy 面不激
/// 活（simple 面两代同源自证，不挂 Era）。BSM / 解密器 / @Metadata 注解清
/// 理不做：改写只动调用点，指纹条目交 DeadMember 引用普查与 metadata-strip
/// 类面。

#include "grunt/indy_restore.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::indy_restore {
namespace {

namespace ir = jbc::ir;
namespace knowledge = grunt::knowledge;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::type::MethodDesc;
using jbc::type::TypeDesc;

constexpr ca::u16 kAccInterface = 0x0200;   ///< class access_flags 的 ACC_INTERFACE

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

/// @brief 方法描述符解析（空参/坏形返回 false，不抛）。
bool parseMethod(std::string_view desc, MethodDesc& out) noexcept
{
    auto r = jbc::type::parseMethodDescriptor(desc);
    if (r.is_err()) return false;
    out = std::move(r).unwrap();
    return true;
}

/// @brief 严格十进制解析（`Integer.parseInt(s, 10)` 直解面）；空串/非数字拒。
bool parseIntStrict(std::string_view s, ca::i64& out)
{
    if (s.empty()) return false;
    ca::usize i   = 0;
    bool     neg  = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i   = 1;
        if (s.size() == 1) return false;
    }
    ca::u64 v = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<ca::u64>(c - '0');
        if (v > 0x7FFFFFFF) return false;   // parseInt 的 int 域
    }
    out = neg ? -static_cast<ca::i64>(v) : static_cast<ca::i64>(v);
    return true;
}

// ============================================================================
// ==== 每类键面收集：解密器窗口与 BSM 键链 ==================================
// ============================================================================

/// @brief 解密器窗口形状（v3 材料通道位）。
enum class DecShape : ca::u8
{
    Plain = 0,     ///< `charAt; LDC K; IXOR; I2C`（或 reobf 折叠键 `LDC A; LDC B;
                   ///  IXOR; IXOR; I2C`，key = A^B）
    Material,      ///< `charAt; LDC K; INVOKESTATIC <self> ()I; IXOR; IXOR; I2C`
                   ///  （v3 RuntimeMaterial 通道：有效 key = K ^ materialKey()，
                   ///  静态不可解——判据只认形状，消费侧 fail-closed）
};

/// @brief 一类一个的解密器键表。
struct ClassKeys
{
    /// @brief 在场解密器：方法名 → 形状 + LDC key（Material 形的 key 是
    ///        异或前的原始 decryptKey——与同类 plain 通道共用一常量，源锚
    ///        createDecryptMethod 两分支同一 `LDC(key)`）。
    std::unordered_map<std::string, std::pair<DecShape, ca::i32>> simpleDecs;
    /// @brief 同名同 desc 的 (String;)String 方法在场但窗口不在场（ACC_NATIVE
    ///        壳 / 外形不符）——名字族推导的家族证据（c2 路前置门）。
    std::unordered_set<std::string> deadStringMethods;
    /// @brief BSM 方法名 → 体首条 `INVOKESTATIC <self> (String;)String` 目标
    ///        名（BSM 体在场时的体跟随路）；BSM 壳死不进表。
    std::unordered_map<std::string, std::string> bsmBodyDec;
    /// @brief heavy BSM 方法名 → 体内 generalKey（`LDC K; INVOKESTATIC
    ///        <self> (String;I)String` 窗口的 K）；壳死不进表。
    std::unordered_map<std::string, ca::i32> heavyBsmKey;
};

/// @brief 解密器窗口提取：定位 `I2C` 回溯两层（对齐 indy_decode 的 key 链
///        提取形状）。@return 形状 + key；无窗口 nullopt。
std::optional<std::pair<DecShape, ca::i32>> scanDecryptKey(ir::ClassSession& s, ir::CodeUnit& cu)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    const std::string self = std::string(s.unit().symbols().str(s.unit().thisClass));
    for (ca::usize p = 0; p < order.size(); ++p) {
        const ir::Insn* in = cu.dom.peek(order[p]);
        if (in == nullptr || in->op != Op::I2C) continue;
        if (p < 1) break;
        const ir::Insn* outer = cu.dom.peek(order[p - 1]);
        if (outer == nullptr || outer->op != Op::IXOR) continue;

        // plain 单层：I2C ← IXOR ← LDC K。
        ca::i32 single = 0;
        if (p >= 2 && const_args::decodeConstInt(s, cu, order, p - 2, single)) {
            return std::make_pair(DecShape::Plain, single);
        }
        if (p < 4) continue;
        const ir::Insn* inner = cu.dom.peek(order[p - 2]);
        if (inner == nullptr || inner->op != Op::IXOR) continue;
        const ir::Insn* mid = cu.dom.peek(order[p - 3]);
        if (mid == nullptr) continue;

        // v3 材料通道：I2C ← IXOR ← IXOR ← INVOKESTATIC <self> ()I ← LDC K。
        if (mid->op == Op::INVOKESTATIC) {
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*mid, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Method
                || sem.desc != "()I" || sem.owner != self) {
                continue;
            }
            ca::i32 raw = 0;
            if (const_args::decodeConstInt(s, cu, order, p - 4, raw)) {
                return std::make_pair(DecShape::Material, raw);
            }
            continue;
        }

        // reobf 折叠键（INT(value^K); INT(K); IXOR）：I2C ← IXOR ← IXOR ←
        // LDC K ← LDC value，key = value ^ K。
        ca::i32 ka = 0;
        ca::i32 kb = 0;
        if (const_args::decodeConstInt(s, cu, order, p - 3, ka) && const_args::decodeConstInt(s, cu, order, p - 4, kb)) {
            return std::make_pair(DecShape::Plain, ka ^ kb);
        }
    }
    return std::nullopt;
}

/// @brief 收集一类的解密器键表与 BSM 体跟随面（纯读；BSM 壳死按缺席记账）。
void collectClassKeys(ir::ClassSession& s, ClassKeys& out)
{
    const ir::ClassUnit& unit = s.unit();
    const std::string    self = std::string(unit.symbols().str(unit.thisClass));

    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        const std::string mname(unit.symbols().str(unit.methods[mi].name));
        const std::string_view mdesc = unit.symbols().str(unit.methods[mi].desc);

        if (mdesc == knowledge::kGruntSimpleDecryptorDesc) {
            if (!ir::hasCode(s, mi)) {
                out.deadStringMethods.insert(mname);
                continue;
            }
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) {
                out.deadStringMethods.insert(mname);
                continue;
            }
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) {
                out.deadStringMethods.insert(mname);
                continue;
            }
            auto key = scanDecryptKey(s, *cu);
            if (key.has_value()) {
                out.simpleDecs.emplace(mname, *key);
            } else {
                out.deadStringMethods.insert(mname);
            }
            continue;
        }
        if (mdesc != knowledge::kGruntSimpleBsmDesc
            && mdesc != knowledge::kGruntHeavyBsmDesc) {
            continue;
        }
        // BSM 体跟随面（壳死 = 无 Code，缺席记账）。
        if (!ir::hasCode(s, mi)) continue;
        auto promoted = ir::promoteMethod(s, mi);
        if (promoted.is_err()) continue;
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;
        const std::vector<ir::InsnId>& order = cu->dom.order();
        for (ca::usize p = 0; p < order.size(); ++p) {
            const ir::Insn* in = cu->dom.peek(order[p]);
            if (in == nullptr) continue;
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*in, sem) || sem.kind != ir::ClassSession::MemberSem::Kind::Method) {
                continue;
            }
            if (mdesc == knowledge::kGruntSimpleBsmDesc) {
                // simple：体内首条直调本类 (String;)String 解密器即映射。
                if (in->op == Op::INVOKESTATIC && sem.owner == self
                    && sem.desc == knowledge::kGruntSimpleDecryptorDesc) {
                    out.bsmBodyDec[mname] = std::string(sem.name);
                    break;
                }
                continue;
            }
            // heavy：`LDC K; INVOKESTATIC <self> (String;I)String` 窗口的 K。
            if (in->op != Op::INVOKESTATIC || sem.owner != self
                || sem.desc != knowledge::kGruntHeavyDecryptorDesc) {
                continue;
            }
            if (p == 0) continue;
            ca::i32 key = 0;
            if (const_args::decodeConstInt(s, *cu, order, p - 1, key)) {
                out.heavyBsmKey[mname] = key;
            }
            break;
        }
    }
}

// ============================================================================
// ==== 键链三路裁决（simple BSM 入口）=======================================
// ============================================================================

/// @brief simple BSM 的键裁决结果。
struct SimpleLane
{
    enum class Kind : ca::u8
    {
        Key,          ///< key 就绪（DecShape::Plain 的产物）
        Material,     ///< 材料通道（运行期材料字段，静态不可解）
        Missing,      ///< 键链三路全断
    };
    Kind            kind = Kind::Missing;
    ca::i32         key  = 0;
};

/// @brief 键链三路：同名直连 → BSM 体跟随 → 名字族推导。裁决按 BSM 方法名
///        （同一方法名共享一个键面——多条 BootstrapMethods 条目指向同一
///        BSM 方法是常态）。
SimpleLane resolveSimpleLane(const std::string& bsmName, const ClassKeys& keys)
{
    SimpleLane lane;

    // 1) 同名直连（v3 material 配对类：plainDecryptName == plainBsmName）。
    const auto direct = keys.simpleDecs.find(bsmName);
    if (direct != keys.simpleDecs.end()) {
        if (direct->second.first == DecShape::Material) {
            lane.kind = SimpleLane::Kind::Material;
            return lane;
        }
        lane.kind = SimpleLane::Kind::Key;
        lane.key  = direct->second.second;
        return lane;
    }

    // 2) BSM 体跟随（v2/v3 常态，BSM 体在场）。
    const auto body = keys.bsmBodyDec.find(bsmName);
    if (body != keys.bsmBodyDec.end()) {
        const auto dec = keys.simpleDecs.find(body->second);
        if (dec != keys.simpleDecs.end()) {
            if (dec->second.first == DecShape::Material) {
                lane.kind = SimpleLane::Kind::Material;
                return lane;
            }
            lane.kind = SimpleLane::Kind::Key;
            lane.key  = dec->second.second;
            return lane;
        }
        return lane;   // 体指到的解密器不在场：keyMissing
    }

    // 3) 名字族推导（v3 NativePipeline 壳现场）。源锚：decryptName =
    //    bsmName1.substring(2, len-1)（grunt3@e41e852 ReferenceObfuscate.kt
    //    L216 / grunt2@36c0eef 同构），即解密器名 = BSM 名去头两字符再去尾
    //    一字符；v3 material 配对的 plain 通道名再缩一格（substring(1,
    //    len-3)），与 material 解密器名恰有「去首字符前缀」关系。
    for (const auto& [decName, shapeKey] : keys.simpleDecs) {
        const ca::usize db = bsmName.size();
        const ca::usize dd = decName.size();
        // c1（无 material 类）：D = B[2, len(B)-1)。plain → key。
        if (shapeKey.first == DecShape::Plain && dd + 3 == db && db >= 3
            && bsmName.compare(2, dd, decName) == 0) {
            lane.kind = SimpleLane::Kind::Key;
            lane.key  = shapeKey.second;
            return lane;
        }
        // material 解密器贴在 BSM 名上（heavy/材料 BSM 壳）→ materialLane。
        if (shapeKey.first == DecShape::Material && dd + 3 == db && db >= 3
            && bsmName.compare(2, dd, decName) == 0) {
            lane.kind = SimpleLane::Kind::Material;
            return lane;
        }
    }
    // c2（material 配对类 + plain 解密器被 native 摘除）：BSM 名与一个壳死
    // 的 (String;)String 同名（家族证据），material 解密器名 M 满足
    // M[0, len(B)-1) == B[1, len(B))——LDC K 即原始 decryptKey（两分支共用
    // 一常量），clinit/init 的 plain 站点用它直解。
    if (keys.deadStringMethods.count(bsmName) != 0) {
        for (const auto& [decName, shapeKey] : keys.simpleDecs) {
            if (shapeKey.first != DecShape::Material) continue;
            const ca::usize db = bsmName.size();
            const ca::usize dm = decName.size();
            if (dm == db + 1 && db >= 2 && decName.compare(0, db - 1, bsmName, 1, db - 1) == 0) {
                lane.kind = SimpleLane::Kind::Key;
                lane.key  = shapeKey.second;   // LDC 原始 key（材料异或前）
                return lane;
            }
        }
    }
    return lane;
}

// ============================================================================
// ==== heavy @Metadata 读取 ==================================================
// ============================================================================

/// @brief 注解元素查找：注解 values 里按元素名取值。
const jbc::core::AnnotationsAttr::ElemValue* findElement(
    const jbc::core::AnnotationsAttr::Annotation& anno, std::string_view name,
    const jbc::core::ConstantPool& cp)
{
    for (const auto& [nameCp, value] : anno.values) {
        if (!cp.valid(nameCp)) continue;
        if (cp.utf8Of(nameCp) == name) return &value;
    }
    return nullptr;
}

/// @brief 元数据数组的字符串项（'[' tag 的 items，每项 's' tag）。
bool readStringArray(const jbc::core::AnnotationsAttr::ElemValue& value,
                     const jbc::core::ConstantPool& cp, std::vector<std::string>& out)
{
    out.clear();
    if (value.tag != '[') return false;
    for (const jbc::core::AnnotationsAttr::ElemValue& item : value.items) {
        if (item.tag != 's' || !cp.valid(item.cp)) return false;
        out.emplace_back(cp.utf8Of(item.cp));
    }
    return true;
}

/// @brief 从 owner 类字节读 @Metadata 形注解（mv/d1/d2 元素名 + mv 版本门 +
///        注解类在容器内的 mv/d1/d2 抽象方法面）。@return 读到填 d1/d2。
bool readMetadata(const std::unordered_map<std::string, const std::vector<ca::u8>*>& classesBySelf,
                  const std::string& ownerInternal, std::vector<std::string>& d1,
                  std::vector<std::string>& d2)
{
    const auto it = classesBySelf.find(ownerInternal);
    if (it == classesBySelf.end()) return false;
    auto session = openSession(*it->second);
    if (!session) return false;
    jbc::core::ClassFile&     cf    = session->backing();
    const jbc::core::ConstantPool& cp = cf.cp;

    // 类级可见注解扫描。
    for (jbc::core::AttrSlot& as : cf.attrs) {
        if (!mjbase::util::attrSafeToDecode(cf, as)) continue;
        auto d = cf.decodeAttr(as);
        if (d.is_err()) continue;
        jbc::core::AttrBody* body = d.unwrap();
        if (body == nullptr || body->kind() != jbc::core::AttrKind::Annotations) continue;
        const auto* aa = static_cast<const jbc::core::AnnotationsAttr*>(body);
        for (const jbc::core::AnnotationsAttr::Annotation& anno : aa->annos) {
            if (!cp.valid(anno.typeCp)) continue;
            const std::string_view typeDesc = cp.utf8Of(anno.typeCp);
            // 形状判据：L<name>; 描述符 + mv/d1/d2 元素名齐备。
            if (typeDesc.size() < 3 || typeDesc.front() != 'L' || typeDesc.back() != ';') {
                continue;
            }
            const jbc::core::AnnotationsAttr::ElemValue* mv = findElement(anno, "mv", cp);
            const jbc::core::AnnotationsAttr::ElemValue* e1 = findElement(anno, "d1", cp);
            const jbc::core::AnnotationsAttr::ElemValue* e2 = findElement(anno, "d2", cp);
            if (mv == nullptr || e1 == nullptr || e2 == nullptr) continue;
            if (mv->tag != 'I' || !cp.valid(mv->cp)) continue;
            const jbc::core::ConstantPool::Entry& mvEntry = cp.at(mv->cp);
            if (mvEntry.tag != CpTag::Integer) continue;
            // mv 版本门（BSM 体 BIPUSH 100; IF_ICMPGE 的静态半边）。
            if (mvEntry.ival < knowledge::kGruntMetadataMv) continue;

            // 注解类结构作证：容器内有该注解类且带 mv/d1/d2 抽象方法三件套。
            std::string annoClass(typeDesc.substr(1, typeDesc.size() - 2));
            const auto cls = classesBySelf.find(annoClass);
            if (cls == classesBySelf.end()) return false;   // 注解类不在场：fail-closed
            auto annoS = openSession(*cls->second);
            if (!annoS) return false;
            const jbc::ir::ClassUnit& au = annoS->unit();
            bool hasMv = false, hasD1 = false, hasD2 = false;
            for (const jbc::ir::MethodUnit& m : au.methods) {
                const std::string_view n = au.symbols().str(m.name);
                const std::string_view v = au.symbols().str(m.desc);
                hasMv = hasMv || (n == "mv" && v == "()I");
                hasD1 = hasD1 || (n == "d1" && v == "()[Ljava/lang/String;");
                hasD2 = hasD2 || (n == "d2" && v == "()[Ljava/lang/String;");
            }
            if (!hasMv || !hasD1 || !hasD2) return false;

            if (!readStringArray(*e1, cp, d1)) return false;
            if (!readStringArray(*e2, cp, d2)) return false;
            return d1.size() == d2.size() && !d1.empty();
        }
    }
    return false;
}

// ============================================================================
// ==== S1 交叉校验：indy desc 与解密结果互证 =================================
// ============================================================================

/// @brief simple/heavy 共用的 desc 互证。v2/v3 生成面把 virtual 的 indy desc
///        首参恒补 `Ljava/lang/Object;`（`desc.replace("(", "(Ljava/lang/
///        Object;")`）——按生成面钉死，比 Kasumi 面的任意引用 receiver 更严。
bool crossCheck(Form form, const MethodDesc& indy, std::string& targetDesc, std::string& why)
{
    auto fail = [&why](const char* reason) {
        why = reason;
        return false;
    };
    MethodDesc target;
    if (!parseMethod(targetDesc, target)) return fail("目标 desc 不是方法描述符");
    if (form == Form::Static) {
        if (!sameParams(target, indy)) return fail("参数不符（static）");
    } else {
        if (indy.params.size() != target.params.size() + 1) {
            return fail("receiver 槽缺失（virtual）");
        }
        if (indy.params[0].kind != TypeDesc::Kind::Class || indy.params[0].array_dims != 0
            || indy.params[0].class_name != "java/lang/Object") {
            return fail("receiver 不是生成面钉死的 Ljava/lang/Object;");
        }
        for (ca::usize i = 0; i < target.params.size(); ++i) {
            if (!sameType(target.params[i], indy.params[i + 1])) {
                return fail("参数不符（virtual）");
            }
        }
    }
    if (target.ret_void != indy.ret_void || (!target.ret_void && !sameType(target.ret, indy.ret))) {
        return fail("返回类型不符");
    }
    return true;
}

// ============================================================================
// ==== 静态参数读取 ==========================================================
// ============================================================================

/// @brief simple 静态参数（4 个）：[String encOwner, String encName,
///        String encDesc, Integer isVirtual]。
struct SimpleArgs
{
    std::string encOwner;
    std::string encName;
    std::string encDesc;
    ca::u32     isVirtual = 0;
};

bool readSimpleArgs(const jbc::core::ConstantPool& cp, const std::vector<ca::u32>& args,
                    SimpleArgs& out)
{
    if (args.size() != 4) return false;
    const auto strAt = [&](ca::u32 cpIndex, std::string& value) {
        if (!cp.valid(cpIndex)) return false;
        const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
        if (e.tag != CpTag::String || !cp.valid(e.a)) return false;
        value = std::string(cp.utf8Of(e.a));
        return true;
    };
    const auto intAt = [&](ca::u32 cpIndex, ca::u32& value) {
        if (!cp.valid(cpIndex)) return false;
        const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
        if (e.tag != CpTag::Integer) return false;
        value = static_cast<ca::u32>(e.ival);
        return true;
    };
    return strAt(args[0], out.encOwner) && strAt(args[1], out.encName)
           && strAt(args[2], out.encDesc) && intAt(args[3], out.isVirtual);
}

/// @brief heavy 静态参数（4 个）：[String encOwner, Integer magic1,
///        Integer magic2, Integer isVirtual]。
struct HeavyArgs
{
    std::string encOwner;
    ca::i32     magic1    = 0;
    ca::i32     magic2    = 0;
    ca::u32     isVirtual = 0;
};

bool readHeavyArgs(const jbc::core::ConstantPool& cp, const std::vector<ca::u32>& args,
                   HeavyArgs& out)
{
    if (args.size() != 4) return false;
    const auto intAt = [&](ca::u32 cpIndex, ca::i32& value) {
        if (!cp.valid(cpIndex)) return false;
        const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
        if (e.tag != CpTag::Integer) return false;
        value = e.ival;
        return true;
    };
    if (!cp.valid(args[0])) return false;
    const jbc::core::ConstantPool::Entry& owner = cp.at(args[0]);
    if (owner.tag != CpTag::String || !cp.valid(owner.a)) return false;
    out.encOwner = std::string(cp.utf8Of(owner.a));
    const auto u32At = [&](ca::u32 cpIndex, ca::u32& value) {
        if (!cp.valid(cpIndex)) return false;
        const jbc::core::ConstantPool::Entry& e = cp.at(cpIndex);
        if (e.tag != CpTag::Integer) return false;
        value = static_cast<ca::u32>(e.ival);
        return true;
    };
    return intAt(args[1], out.magic1) && intAt(args[2], out.magic2)
           && u32At(args[3], out.isVirtual);
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

namespace {

enum class Verdict
{
    Skip,    ///< 非 Grunt indy / CP 不可归属：静默跳过（不进 rejects）
    Accept,  ///< 站点成立
    Reject,  ///< fail-closed 拒绝（进 rejects，bucket 已记账）
};

Verdict judgeSite(const std::string& uri, ca::usize methodIndex,
                  ca::u32 srcPc, const ir::Insn& in,
                  const std::vector<jbc::core::BootstrapMethodsAttr::Entry>& bsms,
                  const jbc::core::ConstantPool& cp,
                  const std::unordered_map<std::string, ClassKeys>& keysByOwner,
                  const std::unordered_map<std::string, const std::vector<ca::u8>*>& classesBySelf,
                  const std::unordered_map<std::string, bool>& jarIsInterface,
                  const ScanInput& input, Site& outSite, Reject& outReject, ScanResult& stats)
{
    const auto reject = [&](std::string why, ca::usize bucket) {
        outReject.uri         = uri;
        outReject.methodIndex = methodIndex;
        outReject.indySrcPc   = srcPc;
        outReject.why         = std::move(why);
        switch (bucket) {
        case 0: ++stats.keyMissing; break;
        case 1: ++stats.malformedArguments; break;
        case 2: ++stats.decodeFailed; break;
        case 3: ++stats.descMismatch; break;
        case 4: ++stats.magicMismatch; break;
        case 5: ++stats.metadataMissing; break;
        case 6: ++stats.materialLane; break;
        default: ++stats.interfaceUnattested; break;
        }
        return Verdict::Reject;
    };

    // CP 不可归属（坏 classfile / 非 indy 语义）：静默跳过，不算 Grunt 账。
    if (!cp.valid(in.cp)) return Verdict::Skip;
    const jbc::core::ConstantPool::Entry& indy = cp.at(in.cp);
    if (indy.tag != CpTag::InvokeDynamic) return Verdict::Skip;
    if (indy.a >= bsms.size()) return Verdict::Skip;

    // ---- S2 第一钉：Grunt simple/heavy BSM desc 指纹 ----
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
    const bool isSimple = bsmDesc == knowledge::kGruntSimpleBsmDesc;
    const bool isHeavy  = bsmDesc == knowledge::kGruntHeavyBsmDesc;
    if (!isSimple && !isHeavy) return Verdict::Skip;
    ++stats.totalIndy;

    // ---- indy desc 解析（S1 互证的另一边）----
    if (!cp.valid(indy.b)) return reject("indy NameAndType 索引无效", 2);
    const jbc::core::ConstantPool::Entry& nat = cp.at(indy.b);
    if (nat.tag != CpTag::NameAndType || !cp.valid(nat.b)) {
        return reject("indy NameAndType 解析失败", 2);
    }
    MethodDesc indyDesc;
    if (!parseMethod(std::string_view(cp.utf8Of(nat.b)), indyDesc)) {
        return reject("indy desc 解析失败", 2);
    }

    Site site;
    site.uri         = uri;
    site.methodIndex = methodIndex;
    site.indySrcPc   = srcPc;
    std::string why;

    if (isSimple) {
        // ---- 键链三路 ----
        const auto kit = keysByOwner.find(bsmOwner);
        if (kit == keysByOwner.end()) return reject("BSM owner 无键表", 0);
        const SimpleLane lane = resolveSimpleLane(bsmName, kit->second);
        if (lane.kind == SimpleLane::Kind::Material) {
            return reject("v3 材料通道解密器（运行期材料字段，静态不可解）", 6);
        }
        if (lane.kind == SimpleLane::Kind::Missing) return reject("键链三路全断（同名/体跟随/名字族）", 0);

        // ---- 静态参数 ----
        SimpleArgs args;
        if (!readSimpleArgs(cp, bsm.args, args)) {
            return reject("静态参数个数/种类不符（需 [String,String,String,Integer]）", 1);
        }
        if (args.isVirtual > 1) return reject("isVirtual 越界（生成面只发 0/1）", 2);
        site.form = args.isVirtual == 1 ? Form::Virtual : Form::Static;

        // ---- 解密三段 ----
        std::string decOwner;
        std::string decName;
        std::string decDesc;
        if (!scan_tools::decryptXor(args.encOwner, lane.key, decOwner)) return reject("owner 解密失败", 2);
        if (!scan_tools::decryptXor(args.encName, lane.key, decName)) return reject("name 解密失败", 2);
        if (!scan_tools::decryptXor(args.encDesc, lane.key, decDesc)) return reject("desc 解密失败", 2);
        if (decOwner.empty()) return reject("owner 解密为空", 2);
        if (decName.empty() || decDesc.empty()) return reject("name/desc 解密为空", 2);
        site.targetOwner = jbc::type::binaryToInternal(decOwner).to_std_string();
        site.targetName  = decName;
        site.targetDesc  = decDesc;

        // ---- S1 互证 ----
        if (!crossCheck(site.form, indyDesc, site.targetDesc, why)) {
            return reject(std::move(why), 3);
        }
    } else {
        // ---- heavy：Era 门（@Metadata 数组解释是 S3 版本敏感面）----
        if (input.era != knowledge::Era::Grunt3) {
            return reject("heavy 面未激活（Era 未判 Grunt3，fail-closed）", 5);
        }
        const auto kit = keysByOwner.find(bsmOwner);
        if (kit == keysByOwner.end()) return reject("BSM owner 无键表", 0);
        const auto gk = kit->second.heavyBsmKey.find(bsmName);
        if (gk == kit->second.heavyBsmKey.end()) {
            return reject("heavy generalKey 缺失（BSM 壳死或体形不符）", 0);
        }

        HeavyArgs args;
        if (!readHeavyArgs(cp, bsm.args, args)) {
            return reject("静态参数个数/种类不符（需 [String,Integer,Integer,Integer]）", 1);
        }
        if (args.isVirtual > 1) return reject("isVirtual 越界（生成面只发 0/1）", 2);
        site.form = args.isVirtual == 1 ? Form::Virtual : Form::Static;

        // ---- owner 解密（BSM 体 LDC generalKey 一层）----
        std::string decOwner;
        if (!scan_tools::decryptXor(args.encOwner, gk->second, decOwner)) return reject("owner 解密失败", 2);
        if (decOwner.empty()) return reject("owner 解密为空", 2);
        site.targetOwner = jbc::type::binaryToInternal(decOwner).to_std_string();

        // ---- owner 类 @Metadata（mv/d1/d2，注解类在容器内作证）----
        std::vector<std::string> d1;
        std::vector<std::string> d2;
        if (!readMetadata(classesBySelf, site.targetOwner, d1, d2)) {
            return reject("owner 类无 mv/d1/d2 形 @Metadata（或注解类不在场）", 5);
        }

        // ---- d1/d2 全扫：data1 % 59557 == magic2 自证命中 ----
        bool hit = false;
        for (ca::usize i = 0; i < d1.size() && !hit; ++i) {
            std::string data1Str;
            if (!scan_tools::decryptXor(d1[i], args.magic2, data1Str)) continue;
            ca::i64 data1 = 0;
            if (!parseIntStrict(data1Str, data1)) continue;
            // JVM IREM 语义：data1 恒非负（59557*q + magic2，q∈[0,1186)），
            // C++ % 同号直除。
            if (data1 % knowledge::kGruntMetadataModulus != args.magic2) continue;
            std::string data2;
            if (!scan_tools::decryptXor(d2[i], args.magic1, data2)) continue;
            const ca::usize sep = data2.find(knowledge::kGruntMetadataPairSep);
            if (sep == std::string::npos) continue;
            const std::string name = data2.substr(0, sep);
            const std::string desc = data2.substr(sep + knowledge::kGruntMetadataPairSep.size());
            if (name.empty() || desc.empty()) continue;
            if (data2.find(knowledge::kGruntMetadataPairSep, sep + 1) != std::string::npos) {
                continue;   // 分隔符多于一个：严格两段，fail-closed
            }
            site.targetName = name;
            site.targetDesc = desc;
            hit             = true;
        }
        if (!hit) return reject("d1 全扫无 data1 % 59557 == magic2 命中", 4);

        // ---- S1 互证 ----
        if (!crossCheck(site.form, indyDesc, site.targetDesc, why)) {
            return reject(std::move(why), 3);
        }
    }

    // ---- 接口旁证（virtual 落点分派）：容器普查优先（权威）；JDK 侧是
    //      穷举普查，成员与否双向作证；两处皆无 = fail-closed 拒绝。
    if (site.form == Form::Virtual) {
        const auto jit = jarIsInterface.find(site.targetOwner);
        if (jit != jarIsInterface.end()) {
            site.ownerIsInterface = jit->second;
        } else if (!input.jdkInterfaces.empty()) {
            site.ownerIsInterface = input.jdkInterfaces.count(site.targetOwner) != 0;
        } else {
            return reject("JDK 接口集未注入，virtual 落点无旁证（fail-closed）：" + site.targetOwner,
                          7);
        }
    }

    outSite = std::move(site);
    return Verdict::Accept;
}

}   // namespace

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    ScanResult result;

    // 第一遍：接口普查 + 每类键表 + 内部名 → 字节映射（heavy owner 读用）。
    std::unordered_map<std::string, bool>                              jarIsInterface;
    std::unordered_map<std::string, ClassKeys>                         keysByOwner;
    std::unordered_map<std::string, const std::vector<ca::u8>*>        classesBySelf;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        const ir::ClassUnit& unit = session->unit();
        const std::string    self = std::string(unit.symbols().str(unit.thisClass));
        jarIsInterface[self]      = (unit.access & kAccInterface) != 0;
        classesBySelf[self]       = entry.bytes;
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

        ir::ClassSession&             s    = *session;
        const jbc::core::ConstantPool& cp   = s.backing().cp;
        const ir::ClassUnit&          unit = s.unit();
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
                const Verdict verdict = judgeSite(uri, mi, in->srcPc, *in, *bsms, cp,
                                                  keysByOwner, classesBySelf, jarIsInterface,
                                                  input, site, rej, result);
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

namespace {

/// @brief 单站点改写。@return 命中形态；nullopt = 未动（failed 记账）。
///
/// v2/v3 只有方法调用两态且都是纯原地（virtual 的 receiver 槽就是 indy 首
/// 参，不插不删），原地换指令保 srcPc。
std::optional<Form> rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu, const Site& site,
                                ca::usize& failed)
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
    // 站点复核：必须还是 indy（防漂移）。
    const jbc::core::ConstantPool& cp = s.backing().cp;
    if (!cp.valid(in->cp) || cp.at(in->cp).tag != CpTag::InvokeDynamic) {
        ++failed;
        return std::nullopt;
    }

    in->op = site.form == Form::Static
                 ? Op::INVOKESTATIC
                 : (site.ownerIsInterface ? Op::INVOKEINTERFACE : Op::INVOKEVIRTUAL);
    const bool ok = s.setMember(
        *in,
        site.form == Form::Static
            ? ir::ClassSession::MemberSem::Kind::Method
            : (site.ownerIsInterface ? ir::ClassSession::MemberSem::Kind::InterfaceMethod
                                     : ir::ClassSession::MemberSem::Kind::Method),
        site.targetOwner, site.targetName, site.targetDesc);
    if (!ok) {
        in->op = Op::INVOKEDYNAMIC;   // 换指令失败：还原，不留半截指令
        ++failed;
        return std::nullopt;
    }
    return site.form;
}

}   // namespace

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    // 站点按 uri 聚类（万类大罐上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           sites = 0;
        std::vector<Form>   forms;
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

        // 前置门：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize         hit   = 0;
        std::vector<Form> forms;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize         failed = 0;
            std::vector<Form> methodForms;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    for (const Site* site : sitesOfMethod) {
                        auto hitForm = rewriteSite(s, cu, *site, failed);
                        if (hitForm.has_value()) methodForms.push_back(*hitForm);
                    }
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

    // 形态计数在类写回成功后归账（类级放弃不许虚报站点）。
    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.sites;
        dirty.classes += 1;
        for (const Form form : item.forms) {
            if (form == Form::Static) {
                ++dirty.rewrittenStatic;
            } else {
                ++dirty.rewrittenVirtual;
            }
        }
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

namespace {

/// @brief 容器里仍带 Grunt simple/heavy 指纹的 indy 调用点数（residual 口径）。
ca::usize countGruntIndyResidual(const mjbase::model::ResourceContainer& container)
{
    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        const auto* bsms = scan_tools::bsmEntries(*session);
        if (bsms == nullptr) return;

        // 指纹条目下标集（proof 只判不猜：desc 全等才算 Grunt BSM）。
        std::set<ca::usize> gruntBsm;
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
            if (nat.tag == CpTag::NameAndType && cp.valid(nat.b)) {
                const std::string_view desc = cp.utf8Of(nat.b);
                if (desc == knowledge::kGruntSimpleBsmDesc
                    || desc == knowledge::kGruntHeavyBsmDesc) {
                    gruntBsm.insert(i);
                }
            }
        }
        if (gruntBsm.empty()) return;

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
                if (indy.tag == CpTag::InvokeDynamic && indy.a < bsms->size()
                    && gruntBsm.count(indy.a) != 0) {
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
    // proof 是效力门（不变量复核），不许被 pass 级时间片截短。
    pass_clock::Suspend suspend;
    ProofResult         result;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    result.ok       = intact.ok;
    result.failed   = intact.failed;
    result.residual = countGruntIndyResidual(container);
    return result;
}

}   // namespace grunt::indy_restore
