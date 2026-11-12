/// @file        const_value_promote.cpp
/// @brief       残留 `const_value_promote` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 数据流
///
/// `Site{uri, clinitIndex, fieldName, fieldDesc, putSrcPc, ValueLoad}` →
/// `DirtySet{uris, promoted, attempted, failed}`。
///
/// ## 值面解码与回填通道
///
/// `const_args::decodeValueLoad`（工具箱）：与栈值面 `decodePush` 是两个
/// 事实——接受 ldc String，拒绝 aconst_null。返回
/// `ValueLoad{kind, bits, str, slots}`，ev::Value 装不下字符串所以独立成
/// 结构。回填三通道：`internInteger`（i32，会话侧 intern）、
/// `cp.addLong/addFloatBits/addDoubleBits`（u2 域检查，LDC2_W 是 CP2）、
/// `addString(addUtf8(...))`。
///
/// ## 复核纪律
///
/// rewrite 按 uri + 字段名 + 描述重定位字段、按 putSrcPc + memberOf 重定位
/// 写入点，**值重新解码并与 ScanResult 携带值按位比对**（kind + bits +
/// str）。scan 之后同方法其他字段可能已改写，不盲改。
#include "grunt/const_value_promote.hpp"

#include <cstring>
#include <set>
#include <string>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/field_census.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::const_value_promote {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic = 0x0008;
constexpr ca::u32 kAccFinal  = 0x0010;

/// @brief 描述符是否是 ConstantValue 合形态（基本类型 / String）。
bool descSupported(std::string_view desc)
{
    if (desc == "Ljava/lang/String;") return true;
    return !desc.empty() && std::strchr("BCISZJFD", desc[0]) != nullptr;
}

/// @brief 值形态与字段描述符是否相容。
bool valueMatches(std::string_view desc, const const_args::ValueLoad& v)
{
    using K = const_args::ValueLoad::Kind;
    if (desc == "Ljava/lang/String;") return v.kind == K::Str;
    if (desc.empty()) return false;
    switch (desc[0]) {
    case 'I': case 'S': case 'B': case 'C': case 'Z': return v.kind == K::I32;
    case 'J': return v.kind == K::I64;
    case 'F': return v.kind == K::F32;
    case 'D': return v.kind == K::F64;
    default: return false;
    }
}

/// @brief 字段是否已带 ConstantValue。
bool fieldHasConstantValue(ir::ClassSession& s, const jbc::core::MemberInfo& fld)
{
    for (const jbc::core::AttrSlot& slot : fld.attrs) {
        if (s.backing().utf8(slot.nameCp) == "ConstantValue") return true;
    }
    return false;
}

/// @brief 常量值进池，返回 CP 索引（0 = 失败）。
ca::u32 internValue(ir::ClassSession& s, const const_args::ValueLoad& v)
{
    jbc::core::ClassFile& cf = s.backing();
    using K                  = const_args::ValueLoad::Kind;
    switch (v.kind) {
    case K::I32: return s.internInteger(v.i32());
    case K::I64: {
        const ca::u32 cp = cf.cp.addLong(v.i64());
        return cp <= 0xFFFF ? cp : 0;
    }
    case K::F32: return cf.cp.addFloatBits(static_cast<ca::u32>(v.bits));
    case K::F64: {
        const ca::u32 cp = cf.cp.addDoubleBits(v.bits);
        return cp <= 0xFFFF ? cp : 0;
    }
    case K::Str: {
        const ca::u32 cp = cf.cp.addString(cf.cp.addUtf8(v.str));
        return cp <= 0xFFFF ? cp : 0;
    }
    default: return 0;
    }
}

/// @brief clinit 里找 <clinit> 下标。
ca::usize findClinit(ir::ClassSession& s)
{
    const ir::ClassUnit& unit = s.unit();
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (unit.symbols().str(unit.methods[mi].name) == "<clinit>" && ir::hasCode(s, mi)) {
            return mi;
        }
    }
    return static_cast<ca::usize>(-1);
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    // 访问面普查不完整（会话打不开的类 / promote 失败的方法）＝引用证据不足
    // ——「唯一写在本类 clinit」判据会漏计漏普查面上的写，fail-closed 整轮
    // 放弃（与 dead_member 引用普查同一课，宁误留不误删）。
    field_census::Census     census;
    const field_census::Stat stat = field_census::scanFieldAccess(container, census);
    if (!stat.complete()) {
        result.censusIncomplete = true;
        std::fprintf(stderr,
                     "[grunt.ConstValuePromote] 普查不完整（类 %zu / 方法 %zu 未普查）——整轮放弃\n",
                     stat.classesSkipped, stat.methodsPromoteFailed);
        return result;
    }

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession& s    = *session;
        jbc::core::ClassFile& cf = s.backing();
        const ca::usize   clinit = findClinit(s);
        if (clinit == static_cast<ca::usize>(-1)) return;
        auto promoted = ir::promoteMethod(s, clinit);
        if (promoted.is_err()) return;
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) return;
        const std::vector<ir::InsnId>& order = cu->dom.order();

        for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
            const jbc::core::MemberInfo& fld = cf.fields[fi];
            if ((fld.access & kAccStatic) == 0) continue;
            const std::string fname(cf.utf8(fld.nameCp));
            const std::string fdesc(cf.utf8(fld.descCp));
            if (!descSupported(fdesc)) continue;
            if (fieldHasConstantValue(s, fld)) continue;

            // 访问面门：全容器唯一一次写，落在本类 clinit（owner::name 键）。
            const std::string ownerKey = std::string(cf.thisName()) + "::" + fname;
            const auto        acc      = census.find(ownerKey);
            if (acc == census.end()) continue;
            ca::usize writes = 0;
            bool writeOk = true;
            for (const field_census::FieldAcc& a : acc->second) {
                if (!a.write) continue;
                ++writes;
                if (a.uri != uri || !a.inClinit) writeOk = false;
            }
            if (!writeOk || writes != 1) continue;

            // clinit 里定位唯一 PUTSTATIC。
            ca::usize putPos = static_cast<ca::usize>(-1);
            for (ca::usize pos = 0; pos < order.size(); ++pos) {
                const ir::Insn* in = cu->dom.peek(order[pos]);
                if (in == nullptr || in->op != Op::PUTSTATIC) continue;
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*in, sem) || sem.name != fname) continue;
                putPos = pos;
                break;
            }
            if (putPos == static_cast<ca::usize>(-1)) continue;

            // 紧邻前驱（NOP 跳过）必须一步产生全部槽位。
            const ir::Insn* v = nullptr;
            for (ca::usize q = putPos; q > 0;) {
                --q;
                const ir::Insn* n = cu->dom.peek(order[q]);
                if (n == nullptr) break;
                if (n->op == Op::NOP) continue;
                v = n;
                break;
            }
            if (v == nullptr) continue;
            const_args::ValueLoad load;
            if (!const_args::decodeValueLoad(s, *v, load)) continue;
            if (load.slots != (fdesc[0] == 'J' || fdesc[0] == 'D' ? 2u : 1u)) continue;
            if (!valueMatches(fdesc, load)) continue;

            Site site;
            site.uri         = uri;
            site.clinitIndex = clinit;
            site.fieldName   = fname;
            site.fieldDesc   = fdesc;
            site.putSrcPc    = v->srcPc;
            site.value       = load;
            result.sites.push_back(std::move(site));
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

    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           fields = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;

        std::vector<const Site*> sitesOf;
        for (const Site& site : result.sites) {
            if (site.uri == uri) sitesOf.push_back(&site);
        }

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        const ca::usize clinit = findClinit(s);
        if (clinit == static_cast<ca::usize>(-1)) return;

        ca::usize hit = 0;
        const bool ok = rewrite_tools::mutateMethodWithFrames(
            s, clinit, frames.resolver(), [&](ir::CodeUnit& cu) {
                jbc::core::ClassFile& cf = s.backing();
                ir::CodeDom& dom = cu.dom;

                for (const Site* site : sitesOf) {
                    ++dirty.attempted;

                    // 字段重定位（scan 后类未改，名字即定位）。
                    jbc::core::MemberInfo* fldPtr = nullptr;
                    for (jbc::core::MemberInfo& f : cf.fields) {
                        if ((f.access & kAccStatic) != 0
                            && cf.utf8(f.nameCp) == site->fieldName
                            && cf.utf8(f.descCp) == site->fieldDesc) {
                            fldPtr = &f;
                            break;
                        }
                    }
                    if (fldPtr == nullptr) {
                        ++dirty.failed;
                        continue;
                    }

                    // putstatic 复核（按值指令 srcPc 定位，后随复核成员与值）。
                    const std::vector<ir::InsnId>& order = dom.order();
                    ca::usize putPos = static_cast<ca::usize>(-1);
                    for (ca::usize pos = 0; pos < order.size(); ++pos) {
                        const ir::Insn* in = dom.peek(order[pos]);
                        if (in == nullptr || in->op != Op::PUTSTATIC) continue;
                        ir::ClassSession::MemberSem sem;
                        if (!s.memberOf(*in, sem) || sem.name != site->fieldName) continue;
                        const ir::Insn* v = nullptr;
                        for (ca::usize q = pos; q > 0;) {
                            --q;
                            const ir::Insn* n = dom.peek(order[q]);
                            if (n == nullptr) break;
                            if (n->op == Op::NOP) continue;
                            v = n;
                            break;
                        }
                        if (v == nullptr || v->srcPc != site->putSrcPc) continue;
                        putPos = pos;
                        break;
                    }
                    if (putPos == static_cast<ca::usize>(-1)) {
                        ++dirty.failed;
                        continue;
                    }

                    // 值指令回溯（NOP 跳过），取位置而非指针——后面要按位改写。
                    ca::usize valuePos = static_cast<ca::usize>(-1);
                    for (ca::usize q = putPos; q > 0;) {
                        --q;
                        const ir::Insn* n = dom.peek(order[q]);
                        if (n == nullptr) break;
                        if (n->op == Op::NOP) continue;
                        valuePos = q;
                        break;
                    }
                    if (valuePos == static_cast<ca::usize>(-1)) {
                        ++dirty.failed;
                        continue;
                    }

                    // 值复核：类别 + 位形/内容一致才动手。
                    const ir::Insn* v = dom.peek(order[valuePos]);
                    const_args::ValueLoad load;
                    if (v == nullptr || !const_args::decodeValueLoad(s, *v, load)
                        || load.kind != site->value.kind || load.bits != site->value.bits
                        || load.str != site->value.str) {
                        ++dirty.failed;
                        continue;
                    }

                    // 标签门：值指令与 putstatic 都不能是合并点。
                    bool blocked = !dom.labelsAt(order[putPos]).empty()
                                   || !dom.labelsAt(order[valuePos]).empty();
                    if (blocked) {
                        ++dirty.failed;
                        continue;
                    }

                    const ca::u32 valueCp = internValue(s, site->value);
                    if (valueCp == 0) {
                        ++dirty.failed;
                        continue;
                    }

                    // 值指令置 NOP（保 srcPc），putstatic 擦除。
                    {
                        auto r = dom.get(order[valuePos]);
                        if (r.is_err()) {
                            ++dirty.failed;
                            continue;
                        }
                        ir::Insn*     vm     = r.unwrap();
                        const ca::u32 keepPc = vm->srcPc;
                        *vm                  = ir::Insn{};
                        vm->op               = Op::NOP;
                        vm->srcPc            = keepPc;
                    }
                    if (dom.eraseBatch({order[putPos]}).is_err()) {
                        ++dirty.failed;
                        continue;
                    }

                    // ACC_FINAL + ConstantValue（RawAttr 2 字节 CP 索引，大端）。
                    fldPtr->access |= kAccFinal;
                    std::vector<ca::u8> payload{static_cast<ca::u8>(valueCp >> 8),
                                                static_cast<ca::u8>(valueCp & 0xFF)};
                    fldPtr->attrs.push_back(jbc::core::AttrSlot::makeNew(
                        cf.cp.addUtf8("ConstantValue"),
                        std::make_unique<jbc::core::RawAttr>(std::move(payload))));
                    ++hit;
                }
                return hit > 0;
            });
        if (!ok) return;   // fail-closed：clinit 放弃，本类不写 pending
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.promoted += item.fields;
    }
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

}   // namespace grunt::const_value_promote
