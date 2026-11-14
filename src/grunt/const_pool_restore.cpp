/// @file        const_pool_restore.cpp
/// @brief       残留 `const_pool_restore` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 数据流
///
/// `FieldValue{owner,name,desc,value}` + `Site{classIdx,methodIndex,srcPc,field}`
/// → `DirtySet{uris,sites,attempted,failed,skippedByRoundTrip}`。
///
/// ## 复核纪律
///
/// rewrite 按 srcPc 重定位 GETSTATIC，成员（owner/name/desc）与站点登记
/// 字段逐一复核后才换常量装载；scan 之后字节可能已被同方法前站改写，
/// 不盲改。字符串值按 owner 类侧已定的明文重照（fields 携带），不在
/// 调用点侧重解。
#include "grunt/const_pool_restore.hpp"

#include <cstring>
#include <set>
#include <string>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/field_census.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/value.hpp"

namespace grunt::const_pool_restore {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using const_args::ValueLoad;
using proof_tools::roundTripSafe;

constexpr ca::u32 kAccStatic    = 0x0008;
constexpr ca::u32 kAccFinal     = 0x0010;
constexpr ca::u32 kAccInterface = 0x0200;
constexpr ca::u32 kAccAbstract  = 0x0400;
constexpr ca::u32 kAccEnum      = 0x4000;
constexpr ca::u32 kAccVolatile  = 0x0040;

/// @brief 描述符是否本面受援（数值四形 + String）。
bool descSupported(std::string_view desc)
{
    if (desc == "Ljava/lang/String;") return true;
    return !desc.empty() && std::strchr("BCISZJFD", desc[0]) != nullptr;
}

std::string fieldKey(std::string_view owner, std::string_view name, std::string_view desc)
{
    std::string key;
    key.reserve(owner.size() + name.size() + desc.size() + 2);
    key += owner;
    key += "::";
    key += name;
    key += ':';
    key += desc;
    return key;
}

/// @brief ConstantValue 属性的值解码（池条目 tag 与描述符相容才收）。
///
/// 两个落地形态都收（与 metadata_strip 同口径）：jbc 原生解码产物与
/// const_value_promote 新建的 2 字节 RawAttr。
bool decodeConstantValue(jbc::core::ClassFile& cf, jbc::core::AttrSlot& slot,
                         std::string_view desc, ValueLoad& out)
{
    auto d = cf.decodeAttr(slot);
    if (d.is_err()) return false;
    const jbc::core::AttrBody* body = d.unwrap();
    if (body == nullptr) return false;
    ca::u32 cpIdx = 0;
    if (body->kind() == jbc::core::AttrKind::ConstantValue) {
        cpIdx = static_cast<const jbc::core::ConstantValueAttr*>(body)->constantValueCp;
    } else if (body->kind() == jbc::core::AttrKind::Raw) {
        const std::vector<ca::u8>& raw = static_cast<const jbc::core::RawAttr*>(body)->bytes();
        if (raw.size() < 2) return false;
        cpIdx = (static_cast<ca::u32>(raw[0]) << 8) | static_cast<ca::u32>(raw[1]);
    } else {
        return false;
    }
    if (!cf.cp.valid(cpIdx)) return false;
    const jbc::core::ConstantPool::Entry& e = cf.cp.at(cpIdx);
    if (desc == "Ljava/lang/String;") {
        if (e.tag != jbc::core::CpTag::String || !cf.cp.valid(e.a)) return false;
        out.kind  = ValueLoad::Kind::Str;
        out.str   = std::string(cf.cp.utf8Of(e.a));
        out.slots = 1;
        return true;
    }
    switch (desc[0]) {
    case 'B': case 'C': case 'I': case 'S': case 'Z':
        if (e.tag != jbc::core::CpTag::Integer) return false;
        out.kind  = ValueLoad::Kind::I32;
        out.bits  = static_cast<ca::u64>(static_cast<ca::i64>(e.ival));
        out.slots = 1;
        return true;
    case 'J':
        if (e.tag != jbc::core::CpTag::Long) return false;
        out.kind  = ValueLoad::Kind::I64;
        out.bits  = static_cast<ca::u64>(e.lval);
        out.slots = 2;
        return true;
    case 'F':
        if (e.tag != jbc::core::CpTag::Float) return false;
        out.kind  = ValueLoad::Kind::F32;
        out.bits  = static_cast<ca::u64>(static_cast<ca::u32>(e.ival));
        out.slots = 1;
        return true;
    case 'D':
        if (e.tag != jbc::core::CpTag::Double) return false;
        out.kind  = ValueLoad::Kind::F64;
        out.bits  = static_cast<ca::u64>(e.lval);
        out.slots = 2;
        return true;
    default: return false;
    }
}

/// @brief 引用判据：调用族指令且 op 与 owner/name/desc 精确吻合。
///
/// 解密器模板的五个成员引用横跨 INVOKESPECIAL（StringBuilder.<init>）与
/// INVOKEVIRTUAL（charAt/append/length/toString），op 逐位钉，不只钉成员三联。
bool isCall(ir::ClassSession& s, const ir::Insn& in, Op op, std::string_view owner,
            std::string_view name, std::string_view desc)
{
    if (in.op != op) return false;
    ir::ClassSession::MemberSem sem;
    if (!s.memberOf(in, sem)) return false;
    return sem.kind == ir::ClassSession::MemberSem::Kind::Method
           && sem.owner == owner && sem.name == name && sem.desc == desc;
}

/// @brief 局部存取归一：紧凑形（ALOAD_1 等）折回一般形（ALOAD）+ 槽位。
///
/// javac/ASM 与 jasm 产物对 0..3 槽走紧凑形（操作数编码在 opcode 里），
/// 生成侧源码面是一般形，两者是同一模板的等价编码。钉形按槽位语义比较，
/// 不钉编码细节；非局部族 op 与槽位原样透传（槽位取操作数 a）。
void normLocal(const ir::Insn& in, Op& op, ca::i32& slot)
{
    const auto fold = [&](Op base, ca::i32 n) {
        op   = base;
        slot = n;
    };
    switch (in.op) {
    case Op::ILOAD_0: fold(Op::ILOAD, 0); return;
    case Op::ILOAD_1: fold(Op::ILOAD, 1); return;
    case Op::ILOAD_2: fold(Op::ILOAD, 2); return;
    case Op::ILOAD_3: fold(Op::ILOAD, 3); return;
    case Op::ALOAD_0: fold(Op::ALOAD, 0); return;
    case Op::ALOAD_1: fold(Op::ALOAD, 1); return;
    case Op::ALOAD_2: fold(Op::ALOAD, 2); return;
    case Op::ALOAD_3: fold(Op::ALOAD, 3); return;
    case Op::ISTORE_0: fold(Op::ISTORE, 0); return;
    case Op::ISTORE_1: fold(Op::ISTORE, 1); return;
    case Op::ISTORE_2: fold(Op::ISTORE, 2); return;
    case Op::ISTORE_3: fold(Op::ISTORE, 3); return;
    case Op::ASTORE_0: fold(Op::ASTORE, 0); return;
    case Op::ASTORE_1: fold(Op::ASTORE, 1); return;
    case Op::ASTORE_2: fold(Op::ASTORE, 2); return;
    case Op::ASTORE_3: fold(Op::ASTORE, 3); return;
    default: slot = in.a; return;
    }
}

/// @brief 解密器体逐字钉：StringBuilder 逐字符 `charAt ^ key` 链。
///
/// 24 条非标签指令的 op 序列（局部存取经 normLocal 归一）+ 5 个成员引用
/// 逐字吻合（java/lang/StringBuilder 与 java/lang/String），槽位钉
/// （sb=1，i=2，实参 0），key 是体内唯一自由变量（LDC int）。生成源单点
/// （v2 InvokeDynamicTransformer.createDecryptMethod）。
bool pinDecryptBody(ir::ClassSession& s, ca::usize methodIndex, ca::i32& key)
{
    if (!ir::hasCode(s, methodIndex)) return false;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return false;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;

    std::vector<const ir::Insn*> in;
    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* cur = cu->dom.peek(id);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        in.push_back(cur);
    }
    if (in.size() != 24) return false;

    static const Op seq[24] = {
        Op::NEW, Op::DUP, Op::INVOKESPECIAL, Op::ASTORE, Op::ICONST_0, Op::ISTORE,
        Op::GOTO,
        Op::ALOAD, Op::ALOAD, Op::ILOAD, Op::INVOKEVIRTUAL, Op::LDC, Op::IXOR,
        Op::I2C, Op::INVOKEVIRTUAL, Op::POP, Op::IINC,
        Op::ILOAD, Op::ALOAD, Op::INVOKEVIRTUAL, Op::IF_ICMPLT,
        Op::ALOAD, Op::INVOKEVIRTUAL, Op::ARETURN,
    };
    Op     ops24[24];
    ca::i32 slots24[24];
    for (ca::usize i = 0; i < 24; ++i) {
        ops24[i]   = in[i]->op;
        slots24[i] = in[i]->a;
        normLocal(*in[i], ops24[i], slots24[i]);
    }
    for (ca::usize i = 0; i < 24; ++i) {
        if (ops24[i] != seq[i]) return false;
    }

    if (!isCall(s, *in[2], Op::INVOKESPECIAL, "java/lang/StringBuilder", "<init>", "()V")) {
        return false;
    }
    if (!isCall(s, *in[10], Op::INVOKEVIRTUAL, "java/lang/String", "charAt", "(I)C")) {
        return false;
    }
    if (!isCall(s, *in[14], Op::INVOKEVIRTUAL, "java/lang/StringBuilder", "append",
                "(C)Ljava/lang/StringBuilder;")) {
        return false;
    }
    if (!isCall(s, *in[19], Op::INVOKEVIRTUAL, "java/lang/String", "length", "()I")) {
        return false;
    }
    if (!isCall(s, *in[22], Op::INVOKEVIRTUAL, "java/lang/StringBuilder", "toString",
                "()Ljava/lang/String;")) {
        return false;
    }

    // NEW 的类是 StringBuilder（memberOf 面 NEW 走类语义，直接查 cp）。
    {
        const jbc::core::ConstantPool& cp = s.backing().cp;
        if (!cp.valid(in[0]->cp)) return false;
        const jbc::core::ConstantPool::Entry& cls = cp.at(in[0]->cp);
        if (cls.tag != jbc::core::CpTag::Class || !cp.valid(cls.a)) return false;
        if (cp.utf8Of(cls.a) != "java/lang/StringBuilder") return false;
    }

    // 槽位钉：ASTORE 1 / ISTORE 2 / ALOAD 1 / ALOAD 0 / ILOAD 2 / IINC 2,+1。
    // 尾环四连是 ILOAD 2(17) ALOAD 0(18) INVOKEVIRTUAL length(19) IF_ICMPLT(20)。
    if (slots24[3] != 1 || slots24[5] != 2) return false;
    if (slots24[7] != 1 || slots24[8] != 0 || slots24[9] != 2) return false;
    if (in[16]->a != 2 || in[16]->b != 1) return false;
    if (slots24[17] != 2 || slots24[18] != 0) return false;
    if (slots24[21] != 1) return false;

    if (!const_args::decodeConstInt(s, *in[11], key)) return false;
    return true;
}

/// @brief 数值面：逐字段判据收 CV 字段，不设类级形状门。
///
/// 判据全部落在字段上：static、非 volatile、受援描述符、全容器零写、
/// 普查有读（零读字段没有可回填的调用点，不登记）、ConstantValue 属性
/// 可解码（池条目 tag 与描述符相容）。运行期值 = CV 属性：罐 B 语料
/// LinkRead 实证 HotSpot 对非 final static 字段照样执行准备期 CV 赋值
/// （3/3 CV-HONORED），GETSTATIC 换 LDC(CV) 语义保持；写面由普查兜底
/// （验证器判死豁免见 field_census 头注）。伴生池（罐 B 观察形：全类
/// static int CV 字段 + 水印方法、无 clinit）只是本面的特例，javac 的
/// static final 常量族同面收走。
void collectNumericFields(ir::ClassSession& s, const field_census::Census& census,
                          ScanResult& result)
{
    jbc::core::ClassFile& cf = s.backing();
    const std::string     self(cf.thisName());

    bool added = false;
    for (jbc::core::MemberInfo& f : cf.fields) {
        const std::string fdesc(cf.utf8(f.descCp));
        if (fdesc == "Ljava/lang/String;") continue;   ///< 字符串面走伴生类门
        if (!descSupported(fdesc)) continue;
        if ((f.access & kAccStatic) == 0) continue;
        if ((f.access & kAccVolatile) != 0) continue;

        const std::string fname(cf.utf8(f.nameCp));
        const auto        acc = census.find(self + "::" + fname);
        if (acc != census.end()) {
            bool wrote  = false;
            bool isRead = false;
            for (const field_census::FieldAcc& a : acc->second) {
                if (a.write) {
                    wrote = true;
                    break;
                }
                isRead = true;
            }
            if (wrote || !isRead) continue;
        } else {
            continue;   ///< 普查零面 = 零读零写，无可回填调用点
        }

        ValueLoad value;
        bool      ok = false;
        for (jbc::core::AttrSlot& slot : f.attrs) {
            if (cf.utf8(slot.nameCp) != "ConstantValue") continue;
            if (decodeConstantValue(cf, slot, fdesc, value)) {
                ok = true;
                ++result.fieldsNumeric;
            }
            break;
        }
        if (!ok) continue;
        result.fieldIndex.emplace(fieldKey(self, fname, fdesc),
                                  static_cast<ca::u32>(result.fields.size()));
        result.fields.push_back(FieldValue{self, fname, fdesc, value});
        added = true;
    }
    if (added) ++result.classesNumeric;
}

/// @brief 水印方法体钉：static ()Ljava/lang/String;，体恰两条 LDC 字符串
/// + ARETURN（罐 B 语料伴生类观察形，"PROTECTED BY GRUNT KLASS MASTER"）。
bool pinWatermarkBody(ir::ClassSession& s, ca::usize methodIndex)
{
    if (!ir::hasCode(s, methodIndex)) return false;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return false;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;
    std::vector<const ir::Insn*> in;
    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* cur = cu->dom.peek(id);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        in.push_back(cur);
    }
    if (in.size() != 2) return false;
    if (in[0]->op != Op::LDC && in[0]->op != Op::LDC_W) return false;
    if (in[1]->op != Op::ARETURN) return false;
    const jbc::core::ConstantPool& cp = s.backing().cp;
    if (!cp.valid(in[0]->cp)) return false;
    const jbc::core::ConstantPool::Entry& e = cp.at(in[0]->cp);
    return e.tag == jbc::core::CpTag::String && cp.valid(e.a);
}

/// @brief 字符串面伴生类形状门 + 字段值面收集。@return 是否过门（fields 追加）。
bool collectPoolClass(ir::ClassSession& s, const std::string& uri,
                      const field_census::Census& census, ScanResult& result)
{
    const ir::ClassUnit&  unit = s.unit();
    jbc::core::ClassFile& cf   = s.backing();

    // 类形状门：具体类（非 interface/abstract/enum）、super = Object。
    // 不钉 ACC_FINAL：生成源版本钉 final，罐 B 语料实测伴生类是普通
    // public 类（形状以罐为准）。
    const ca::u32 access = unit.access;
    if ((access & (kAccInterface | kAccAbstract | kAccEnum)) != 0) return false;
    if (unit.symbols().str(unit.superClass) != "java/lang/Object") return false;

    // 字段门：全部 static + 受援描述符。
    for (const jbc::core::MemberInfo& f : cf.fields) {
        if ((f.access & kAccStatic) == 0) return false;
        if ((f.access & kAccVolatile) != 0) return false;
        if (!descSupported(cf.utf8(f.descCp))) return false;
    }
    if (cf.fields.empty()) return false;

    // 方法门：<clinit> + 解密器（static (String)String，体钉 24 条模板）
    // + 水印方法（static ()String，体钉 LDC 字符串 + ARETURN）。
    std::unordered_map<std::string, ca::i32> decryptKeys;
    ca::usize clinitIndex = static_cast<ca::usize>(-1);
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        const std::string mname(unit.symbols().str(unit.methods[mi].name));
        const std::string_view mdesc(unit.symbols().str(unit.methods[mi].desc));
        if (mname == "<clinit>") {
            clinitIndex = mi;
            continue;
        }
        if ((unit.methods[mi].access & kAccStatic) == 0) return false;
        if (mdesc == "(Ljava/lang/String;)Ljava/lang/String;") {
            ca::i32 key = 0;
            if (!pinDecryptBody(s, mi, key)) return false;
            decryptKeys.emplace(mname, key);
            continue;
        }
        if (mdesc == "()Ljava/lang/String;") {
            if (!pinWatermarkBody(s, mi)) return false;
            continue;
        }
        return false;
    }

    const std::string self(cf.thisName());

    // clinit 提升一次，供字符串字段写点定位。
    std::vector<ir::InsnId> clinitOrder;
    ir::CodeUnit*           clinitCu = nullptr;
    if (clinitIndex != static_cast<ca::usize>(-1)) {
        auto promoted = ir::promoteMethod(s, clinitIndex);
        if (promoted.is_err()) return false;
        clinitCu = promoted.unwrap().get();
        if (clinitCu != nullptr) clinitOrder = clinitCu->dom.order();
    }

    ca::usize added = 0;
    for (jbc::core::MemberInfo& f : cf.fields) {
        const std::string fname(cf.utf8(f.nameCp));
        const std::string fdesc(cf.utf8(f.descCp));
        if (fdesc != "Ljava/lang/String;") continue;   ///< 数值面已逐字段收走

        // 写普查（key = owner::name，与 field_census 同键）。
        const std::string ckey = self + "::" + fname;
        const auto        acc  = census.find(ckey);
        ca::usize         writes = 0;
        bool              writesAllInClinit = true;
        if (acc != census.end()) {
            for (const field_census::FieldAcc& a : acc->second) {
                if (!a.write) continue;
                ++writes;
                if (a.uri != uri || !a.inClinit) writesAllInClinit = false;
            }
        }

        ValueLoad value;
        bool      ok = false;
        // 字符串面：恰一次写、在本类 clinit、写点三连形 + 解密器直解。
        if (writes == 1 && writesAllInClinit && clinitCu != nullptr) {
            for (ca::usize pos = 0; pos < clinitOrder.size(); ++pos) {
                const ir::Insn* in = clinitCu->dom.peek(clinitOrder[pos]);
                if (in == nullptr || in->op != Op::PUTSTATIC) continue;
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*in, sem) || sem.owner != self || sem.name != fname) continue;

                // 前两条非 NOP：INVOKESTATIC 解密器、LDC 密文。
                const ir::Insn* prev1 = nullptr;
                const ir::Insn* prev2 = nullptr;
                for (ca::usize q = pos; q > 0;) {
                    --q;
                    const ir::Insn* n = clinitCu->dom.peek(clinitOrder[q]);
                    if (n == nullptr) break;
                    if (n->op == Op::NOP) continue;
                    if (prev1 == nullptr) {
                        prev1 = n;
                    } else {
                        prev2 = n;
                        break;
                    }
                }
                if (prev1 == nullptr || prev2 == nullptr) break;
                ir::ClassSession::MemberSem dsem;
                if (prev1->op != Op::INVOKESTATIC || !s.memberOf(*prev1, dsem)
                    || dsem.owner != self
                    || dsem.desc != "(Ljava/lang/String;)Ljava/lang/String;") {
                    break;
                }
                const auto keyIt = decryptKeys.find(std::string(dsem.name));
                if (keyIt == decryptKeys.end()) break;
                if (prev2->op != Op::LDC && prev2->op != Op::LDC_W) break;
                const jbc::core::ConstantPool& cp = cf.cp;
                if (!cp.valid(prev2->cp)) break;
                const jbc::core::ConstantPool::Entry& e = cp.at(prev2->cp);
                if (e.tag != jbc::core::CpTag::String || !cp.valid(e.a)) break;
                std::string plain;
                if (!scan_tools::decryptXor(cp.utf8Of(e.a), keyIt->second, plain)) break;

                value.kind  = ValueLoad::Kind::Str;
                value.str   = std::move(plain);
                value.slots = 1;
                ok          = true;
                break;
            }
        }
        if (ok) ++result.fieldsString;

        if (!ok) continue;
        result.fieldIndex.emplace(fieldKey(self, fname, fdesc),
                                  static_cast<ca::u32>(result.fields.size()));
        result.fields.push_back(FieldValue{self, fname, fdesc, value});
        ++added;
    }

    if (added == 0) return false;
    ++result.poolClasses;
    return true;
}

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

/// @brief 值 → 常量装载指令（字符串 intern 进池；数值走 makeConstLoad）。
bool makeLoadInsn(ir::ClassSession& s, const ValueLoad& v, ir::Insn& out)
{
    if (v.kind == ValueLoad::Kind::Str) {
        jbc::core::ClassFile& cf = s.backing();
        const ca::u32 strCp = cf.cp.addString(cf.cp.addUtf8(v.str));
        if (strCp == 0) return false;
        out = ir::CodeEditor::loadConst(strCp);
        return out.op == Op::LDC || out.op == Op::LDC_W;
    }
    return rewrite_tools::makeConstLoad(s, simOf(v), out);
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& /*budgetTemplate*/)
{
    ScanResult result;

    field_census::Census     census;
    const field_census::Stat stat = field_census::scanFieldAccess(container, census);
    result.verifierDead = stat.methodsVerifierDead;
    if (!stat.complete()) {
        result.censusIncomplete = true;
        std::fprintf(stderr,
                     "[grunt.ConstPoolRestore] 普查不完整（类 %zu / 方法 %zu 未普查），整轮放弃\n",
                     stat.classesSkipped, stat.methodsPromoteFailed);
        return result;
    }

    // 面 1：数值面逐字段（无类级门）+ 字符串面伴生类门。
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            return;
        }
        collectNumericFields(*session, census, result);
        collectPoolClass(*session, uri, census, result);
    });

    if (result.fields.empty()) return result;

    // 面 2：调用点登记。
    ca::u32 classIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = classIdx++;
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
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
                if (in == nullptr || in->op != Op::GETSTATIC) continue;
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*in, sem)) continue;
                if (sem.kind != ir::ClassSession::MemberSem::Kind::Field) continue;
                const auto it =
                    result.fieldIndex.find(fieldKey(sem.owner, sem.name, sem.desc));
                if (it == result.fieldIndex.end()) continue;
                result.sites.push_back(Site{idx, mi, in->srcPc, it->second});
            }
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& /*budgetTemplate*/)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    rewrite_tools::FrameContext frames(container);

    std::unordered_map<ca::u32, std::vector<const Site*>> byIdx;
    for (const Site& site : result.sites) byIdx[site.classIdx].push_back(&site);

    ca::u32 curIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = curIdx++;
        const auto    it  = byIdx.find(idx);
        if (it == byIdx.end()) return;
        const std::vector<const Site*> sitesOf = it->second;
        byIdx.erase(it);

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit    = 0;
        ca::usize failed = 0;
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
            ca::usize methodHit = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    const std::vector<ir::InsnId>& order = cu.dom.order();
                    for (const Site* site : sitesOfMethod) {
                        ca::usize pos = static_cast<ca::usize>(-1);
                        for (ca::usize p = 0; p < order.size(); ++p) {
                            const ir::Insn* in = cu.dom.peek(order[p]);
                            if (in != nullptr && in->srcPc == site->srcPc
                                && in->op == Op::GETSTATIC) {
                                pos = p;
                                break;
                            }
                        }
                        if (pos == static_cast<ca::usize>(-1)) {
                            ++failed;
                            ++dirty.failedRelocate;
                            continue;
                        }
                        // 成员复核：owner/name/desc 与登记字段一致才动。
                        const ir::Insn* in = cu.dom.peek(order[pos]);
                        ir::ClassSession::MemberSem sem;
                        const FieldValue&           fv = result.fields[site->field];
                        if (!s.memberOf(*in, sem)
                            || sem.kind != ir::ClassSession::MemberSem::Kind::Field
                            || sem.owner != fv.owner || sem.name != fv.name
                            || sem.desc != fv.desc) {
                            ++failed;
                            ++dirty.failedMember;
                            continue;
                        }

                        auto slotR = cu.dom.get(order[pos]);
                        if (slotR.is_err()) {
                            ++failed;
                            continue;
                        }
                        ir::Insn* insn    = slotR.unwrap();
                        const ca::u32 pc = insn->srcPc;
                        ir::Insn   fresh{};
                        if (!makeLoadInsn(s, fv.value, fresh)) {
                            ++failed;
                            ++dirty.failedMakeLoad;
                            continue;
                        }
                        fresh.srcPc = pc;
                        *insn       = fresh;
                        ++methodHit;
                    }
                    return methodHit > 0;
                });
            if (!ok) {
                failed += methodHit;
                dirty.failedFrames += methodHit;
                continue;
            }
            hit += methodHit;
        }
        dirty.failed += failed;
        if (hit == 0) return;

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

}   // namespace grunt::const_pool_restore
