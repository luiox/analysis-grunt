/// @file        concat_restore.cpp
/// @brief       残留 `concat_restore` 三段实现：SCAN（BSM 包装形状判定 +
///              recipe 回填预演）/ REWRITE（句柄改指 JDK 标准形）/ PROOF
///              （类数 + 可重解析 + residual）。形态与判据见头文件。
#include "grunt/concat_restore.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::concat_restore {
namespace {

using jbc::core::AttrSlot;
using jbc::core::BootstrapMethodsAttr;
using jbc::core::ClassFile;
using jbc::core::CpTag;

constexpr ca::u8 kRefInvokeStatic = 6;   ///< JVMS 4.4.8 REF_invokeStatic

/// @brief 拼接合成串的 UTF-8 字节视图（CP Utf8 内容，recipe 的 tag 字节
///        0x01/0x02 落 ASCII 单字节，多字节字面量不与之重叠）。
std::string_view utf8View(const ClassFile& cf, ca::u32 cpIdx)
{
    if (!cf.cp.valid(cpIdx)) return {};
    const jbc::core::ConstantPool::Entry& e = cf.cp.at(cpIdx);
    if (e.tag != CpTag::Utf8) return {};
    return std::string_view(reinterpret_cast<const char*>(e.utf8), e.utf8Len);
}

/// @brief BSM 条目句柄 → 本类包装方法名（结构不符返回空）。
std::string wrapperMethodName(const ClassFile& cf, const BootstrapMethodsAttr::Entry& e)
{
    if (e.args.size() != 1) return {};
    if (!cf.cp.valid(e.methodRef)) return {};
    const jbc::core::ConstantPool::Entry& mh = cf.cp.at(e.methodRef);
    if (mh.tag != CpTag::MethodHandle || mh.a != kRefInvokeStatic) return {};
    if (!cf.cp.valid(mh.b)) return {};
    const jbc::core::ConstantPool::Entry& ref = cf.cp.at(mh.b);
    if (ref.tag != CpTag::Methodref) return {};
    // owner 必须是本类（自管包装），name/desc 取 NameAndType。
    if (!cf.cp.valid(ref.a) || !cf.cp.valid(ref.b)) return {};
    const jbc::core::ConstantPool::Entry& owner = cf.cp.at(ref.a);
    if (owner.tag != CpTag::Class) return {};
    const std::string_view ownerName = utf8View(cf, owner.a);
    if (ownerName.empty()) return {};
    if (!cf.cp.valid(cf.thisClass)) return {};
    if (ownerName != cf.className(cf.thisClass)) return {};
    const jbc::core::ConstantPool::Entry& nat = cf.cp.at(ref.b);
    if (nat.tag != CpTag::NameAndType) return {};
    const std::string_view name = utf8View(cf, nat.a);
    const std::string_view desc = utf8View(cf, nat.b);
    if (desc != "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                "Ljava/lang/invoke/MethodType;Ljava/lang/String;)"
                "Ljava/lang/invoke/CallSite;") {
        return {};
    }
    return std::string(name);
}

/// @brief 包装体逐字节形状判定：aload×4 → [iconst_N; anewarray Object;
///        N×(dup; iconst_i; ldc String; aastore)] → invokestatic
///        makeConcatWithConstants 五参形 → areturn。@return 常量 String
///        CP 序表（aastore 序），形状不符返回空表且 isWrapper=false。
bool parseWrapperBody(const ClassFile& cf, const std::vector<ca::u8>& code,
                      std::vector<ca::u32>& constants)
{
    ca::usize i = 0;
    auto u1 = [&]() -> ca::u32 { return i < code.size() ? code[i++] : 0x100; };
    auto u2 = [&]() -> ca::u32 {
        if (i + 1 >= code.size()) { i = code.size() + 1; return 0; }
        const ca::u32 v = (static_cast<ca::u32>(code[i]) << 8) | code[i + 1];
        i += 2;
        return v;
    };
    // aload_0..3（0x2a..0x2d）；jasm 亦可能落显式 aload（0x19 + u1 下标）。
    for (ca::u32 k = 0; k < 4; ++k) {
        const ca::u32 op = u1();
        if (op == 0x2a + k) continue;
        if (op == 0x19 && u1() == k) continue;
        return false;
    }
    // iconst_N（0x02..0x08）/ bipush / sipush → 常量数组长度 N。
    ca::i32 arrayLen = 0;
    {
        const ca::u32 op = u1();
        if (op >= 0x03 && op <= 0x08) {
            arrayLen = static_cast<ca::i32>(op) - 0x03;   // iconst_0..5 = 0x03..0x08
        } else if (op == 0x10) {
            arrayLen = static_cast<ca::i8>(u1());
        } else if (op == 0x11) {
            arrayLen = static_cast<ca::i16>(u2());
        } else {
            return false;
        }
        if (arrayLen < 0 || arrayLen > 64) return false;
    }
    if (u1() != 0xbd) return false;   // anewarray
    const ca::u32 arrCls = u2();
    if (!cf.cp.valid(arrCls)) return false;
    const jbc::core::ConstantPool::Entry& arrClsE = cf.cp.at(arrCls);
    if (arrClsE.tag != CpTag::Class || utf8View(cf, arrClsE.a) != "java/lang/Object") return false;
    for (ca::i32 k = 0; k < arrayLen; ++k) {
        if (u1() != 0x59) return false;   // dup
        {
            const ca::u32 op = u1();
            if (op >= 0x03 && op <= 0x08) {
                // iconst 立即数，应为 k
            } else if (op == 0x10) {
                (void)u1();
            } else if (op == 0x11) {
                (void)u2();
            } else {
                return false;
            }
        }
        const ca::u32 ldOp = u1();
        ca::u32 strCp = 0;
        if (ldOp == 0x12) {
            strCp = u1();
        } else if (ldOp == 0x13) {
            strCp = u2();
        } else {
            return false;
        }
        if (!cf.cp.valid(strCp)) return false;
        if (cf.cp.at(strCp).tag != CpTag::String) return false;
        if (u1() != 0x53) return false;   // aastore
        constants.push_back(strCp);
    }
    if (u1() != 0xb8) return false;   // invokestatic
    const ca::u32 tail = u2();
    if (!cf.cp.valid(tail)) return false;
    const jbc::core::ConstantPool::Entry& call = cf.cp.at(tail);
    if (call.tag != CpTag::Methodref) return false;
    if (!cf.cp.valid(call.a) || !cf.cp.valid(call.b)) return false;
    if (utf8View(cf, cf.cp.at(call.a).a) != "java/lang/invoke/StringConcatFactory") return false;
    const jbc::core::ConstantPool::Entry& nat = cf.cp.at(call.b);
    if (nat.tag != CpTag::NameAndType) return false;
    if (utf8View(cf, nat.a) != "makeConcatWithConstants") return false;
    if (utf8View(cf, nat.b)
        != "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
           "MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;") {
        return false;
    }
    if (u1() != 0xb0) return false;   // areturn
    return i == code.size();
}

/// @brief recipe 回填：\u0002 位逐个替换为常量串内容（makeConcatWithConstants
///        对 \u0002 的解析就是按序吃 varargs，回填产出与包装转发同一 recipe）。
/// @return 空串 = 判据不齐（\u0002 数与常量数不等）。
std::string mergeRecipe(const ClassFile& cf, ca::u32 argCp,
                        const std::vector<ca::u32>& constants)
{
    if (!cf.cp.valid(argCp) || cf.cp.at(argCp).tag != CpTag::String) return {};
    const std::string_view tags = utf8View(cf, cf.cp.at(argCp).a);
    ca::usize slots = 0;
    for (const char c : tags) {
        if (c == '\x02') ++slots;
    }
    if (slots != constants.size()) return {};
    std::string out;
    out.reserve(tags.size() + 16 * constants.size());
    ca::usize next = 0;
    for (const char c : tags) {
        if (c == '\x02') {
            out += utf8View(cf, cf.cp.at(constants[next]).a);
            ++next;
        } else {
            out.push_back(c);
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
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ClassFile& cf = session->backing();
        const std::vector<BootstrapMethodsAttr::Entry>* bsms = scan_tools::bsmEntries(*session);
        if (bsms == nullptr) return;
        result.totalBsmEntries += bsms->size();
        for (ca::usize k = 0; k < bsms->size(); ++k) {
            const BootstrapMethodsAttr::Entry& e = (*bsms)[k];
            const std::string wrapperName = wrapperMethodName(cf, e);
            if (wrapperName.empty()) continue;   // 非包装形（Lambda/JDK/其他），不在视野
            // 找包装方法体。
            bool found = false;
            for (jbc::core::MemberInfo& m : cf.methods) {
                if (cf.utf8(m.nameCp) != wrapperName) continue;
                found = true;
                bool        haveCode = false;
                std::vector<ca::u8> code;
                for (AttrSlot& slot : m.attrs) {
                    if (cf.utf8(slot.nameCp) != "Code") continue;
                    auto d = cf.decodeAttr(slot);
                    if (d.is_err()) break;
                    const jbc::core::AttrBody* body = d.unwrap();
                    if (body == nullptr || body->kind() != jbc::core::AttrKind::Code) break;
                    code     = static_cast<const jbc::core::CodeAttr*>(body)->code;
                    haveCode = true;
                    break;
                }
                if (!haveCode) {
                    ++result.wrapperShapeBad;
                    break;
                }
                std::vector<ca::u32> constants;
                if (!parseWrapperBody(cf, code, constants)) {
                    ++result.wrapperShapeBad;
                    break;
                }
                const std::string merged = mergeRecipe(cf, e.args[0], constants);
                if (merged.empty()) {
                    ++result.recipeMismatch;
                    break;
                }
                Site site;
                site.uri         = std::string(entry.uri);
                site.entryIndex  = k;
                site.mergedRecipe = std::move(merged);
                result.sites.push_back(std::move(site));
                break;
            }
            if (!found) ++result.wrapperShapeBad;
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
    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++dirty.failed;
            return;
        }
        ClassFile& cf = session->backing();
        AttrSlot*  slot = cf.findClassAttr("BootstrapMethods");
        if (slot == nullptr) {
            ++dirty.failed;
            return;
        }
        auto decoded = cf.decodeAttr(*slot);
        if (decoded.is_err()) {
            ++dirty.failed;
            return;
        }
        auto* body = decoded.unwrap();
        if (body == nullptr || body->kind() != jbc::core::AttrKind::BootstrapMethods) {
            ++dirty.failed;
            return;
        }

        // 该类的还原清单（entryIndex → mergedRecipe）。
        std::vector<std::pair<ca::usize, const std::string*>> plan;
        for (const Site& site : result.sites) {
            if (site.uri == uri) plan.emplace_back(site.entryIndex, &site.mergedRecipe);
        }

        auto* bsm = ca::core::tag_cast::cast<BootstrapMethodsAttr>(body);
        ca::usize done = 0;
        for (const auto& [k, merged] : plan) {
            if (k >= bsm->entries.size()) continue;
            // 复核：条目还是包装形（scan 之后字节可能已变）。
            if (wrapperMethodName(cf, bsm->entries[k]).empty()) continue;
            // 句柄 → javac 规范形 makeConcatWithConstants（五参 varargs）。
            const ca::u32 nameCp  = cf.cp.addUtf8("makeConcatWithConstants");
            // javac 规范形是五参 varargs 描述符（ CFR 0.152 的折叠匹配按它）；
            // 无常量时 JVM 把零长尾包成空数组，语义不变。
            const ca::u32 descCp  = cf.cp.addUtf8(
                "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                "Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)"
                "Ljava/lang/invoke/CallSite;");
            const ca::u32 clsCp   = cf.cp.addClass(cf.cp.addUtf8(
                "java/lang/invoke/StringConcatFactory"));
            const ca::u32 natCp   = cf.cp.addNameAndType(nameCp, descCp);
            const ca::u32 refCp   = cf.cp.addRef(CpTag::Methodref, clsCp, natCp);
            const ca::u32 mhCp    = cf.cp.addMethodHandle(kRefInvokeStatic, refCp);
            const ca::u32 recCp   = cf.cp.addString(cf.cp.addUtf8(*merged));
            if (mhCp == 0 || recCp == 0 || mhCp > 0xFFFF || recCp > 0xFFFF) continue;
            (void)slot->mutate();   // 声明落盘 dirty（body 与 decoded 同源）
            bsm->entries[k].methodRef = mhCp;
            bsm->entries[k].args      = {recCp};
            ++done;
        }
        if (done == 0) {
            ++dirty.failed;
            return;
        }
        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) {
            dirty.failed += done;
            return;
        }
        replaceClass(container, uri, std::move(encoded).unwrap());
        dirty.uris.push_back(uri);
        dirty.entries += done;
        dirty.classes += 1;
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
    // residual：还原后容器里仍是包装形的条目数（同判据重钉）。
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ClassFile& cf = session->backing();
        const std::vector<BootstrapMethodsAttr::Entry>* bsms = scan_tools::bsmEntries(*session);
        if (bsms == nullptr) return;
        for (const BootstrapMethodsAttr::Entry& e : *bsms) {
            if (wrapperMethodName(cf, e).empty()) continue;
            std::vector<ca::u32> constants;
            bool matched = false;
            const std::string name = wrapperMethodName(cf, e);
            for (jbc::core::MemberInfo& m : cf.methods) {
                if (cf.utf8(m.nameCp) != name) continue;
                for (AttrSlot& slot : m.attrs) {
                    if (cf.utf8(slot.nameCp) != "Code") continue;
                    auto d = cf.decodeAttr(slot);
                    if (d.is_err()) break;
                    const jbc::core::AttrBody* body = d.unwrap();
                    if (body == nullptr || body->kind() != jbc::core::AttrKind::Code) break;
                    matched = parseWrapperBody(
                        cf, static_cast<const jbc::core::CodeAttr*>(body)->code, constants);
                    break;
                }
                break;
            }
            if (matched) ++result.residual;
        }
    });
    return result;
}

}   // namespace grunt::concat_restore
