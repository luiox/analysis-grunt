/// @file        dead_member.cpp
/// @brief       残留 `dead_member` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/dead_member.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "grunt/container.hpp"
#include "grunt/indy_fold.hpp"
#include "grunt/proof_toolbox.hpp"

namespace grunt::dead_member {

namespace {

namespace ir = jbc::ir;

constexpr ca::u32 kAccPrivate    = 0x0002;
constexpr ca::u32 kAccStatic     = 0x0008;
constexpr ca::u32 kAccBridge     = 0x0040;
constexpr ca::u32 kAccNative     = 0x0100;
constexpr ca::u32 kAccAbstract   = 0x0400;
constexpr ca::u32 kAccSynthetic  = 0x1000;

std::string cpUtf8(const jbc::core::ClassFile& cf, ca::u32 i)
{
    return std::string(cf.utf8(i));
}

/// @brief 方法体纯算术形态（grunt 常量算术辅助的形状事实）。
///
/// LDC 单独查数值类别（String/Class 混入即非纯算术）；其余按操作码区间：
/// nop/const/locals、store 族、栈操作、算术/转换/比较、if/icmp/goto、return。
bool pureArithShape(ir::ClassSession& s, ir::CodeUnit& cu)
{
    using jbc::core::Op;
    for (const ir::InsnId id : cu.dom.order()) {
        const ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        const int op = static_cast<int>(in->op);
        if (op >= 0x12 && op <= 0x13) {
            ir::ClassSession::ConstSem sem;
            if (!s.constOf(*in, sem)) return false;
            using K = ir::ClassSession::ConstSem::Kind;
            if (sem.kind != K::Int && sem.kind != K::Long && sem.kind != K::Float
                && sem.kind != K::Double) {
                return false;
            }
            continue;
        }
        const bool ok = (op >= 0x00 && op <= 0x2d)     // nop/const/locals（ldc 已单独查）
                        || (op >= 0x36 && op <= 0x4e)  // store 族
                        || (op >= 0x57 && op <= 0x5f)  // 栈操作
                        || (op >= 0x60 && op <= 0x98)  // 算术/转换/比较
                        || (op >= 0x99 && op <= 0xa7)  // if/icmp/goto
                        || (op >= 0xac && op <= 0xb1); // return 族
        if (!ok) return false;
    }
    return true;
}

/// @brief 字段是否已带 ConstantValue（带 = 不删）。
bool fieldHasConstantValue(ir::ClassSession& s, const jbc::core::MemberInfo& fld)
{
    for (const jbc::core::AttrSlot& slot : fld.attrs) {
        if (s.backing().utf8(slot.nameCp) == "ConstantValue") return true;
    }
    return false;
}

/// @brief 递归收集注解元素值里的字符串面（保守保留依据）。
///
/// 's' 字符串值、'e' 枚举常量名、'c' 类描述符都收——运行时框架（mixin 等）
/// 按这些名字反射定位成员，CP 的 Methodref 静态面看不见这类引用。
void collectAnnoStrings(const jbc::core::AnnotationsAttr::ElemValue& v,
                        const jbc::core::ClassFile& cf, std::set<std::string>& out)
{
    if (v.tag == 's' || v.tag == 'c') out.insert(cpUtf8(cf, v.cp));
    if (v.tag == 'e') out.insert(cpUtf8(cf, v.constNameCp));
    for (const auto& item : v.items) collectAnnoStrings(item, cf, out);
    if (v.nested != nullptr) {
        for (const auto& [nameCp, ev] : v.nested->values) {
            (void)nameCp;
            collectAnnoStrings(ev, cf, out);
        }
    }
}

/// @brief 类内全部注解字符串值：class/field/method 级 Runtime{Visible,
/// Invisible}Annotations 递归展开。
std::set<std::string> annotationStrings(ir::ClassSession& s)
{
    std::set<std::string> out;
    jbc::core::ClassFile& cf = s.backing();

    const auto walkAttrs = [&](std::vector<jbc::core::AttrSlot>& attrs) {
        for (jbc::core::AttrSlot& slot : attrs) {
            const std::string name(cf.utf8(slot.nameCp));
            if (name != "RuntimeVisibleAnnotations" && name != "RuntimeInvisibleAnnotations") {
                continue;
            }
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) continue;
            auto* anno = ca::core::tag_cast::dyn_cast<jbc::core::AnnotationsAttr>(d.unwrap());
            if (anno == nullptr) continue;
            for (const auto& a : anno->annos) {
                for (const auto& [nameCp, ev] : a.values) {
                    (void)nameCp;
                    collectAnnoStrings(ev, cf, out);
                }
            }
        }
    };
    walkAttrs(cf.attrs);
    for (auto& f : cf.fields) walkAttrs(f.attrs);
    for (auto& m : cf.methods) walkAttrs(m.attrs);
    return out;
}

/// @brief 压缩后 CP 的活引用快照：全容器枚举 Fieldref/Methodref/接口引用。
struct CpSnap
{
    std::set<std::string> methodKeys;   // owner::name::desc
    std::set<std::string> fieldKeys;    // owner::name
};

CpSnap collectCpSnap(ir::ClassSession& s)
{
    CpSnap                     out;
    const jbc::core::ClassFile& cf = s.backing();
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (!cf.cp.valid(i)) continue;
        const auto& e = cf.cp.at(i);
        using T       = jbc::core::CpTag;
        if (e.tag != T::Fieldref && e.tag != T::Methodref
            && e.tag != T::InterfaceMethodref) {
            continue;
        }
        const std::string owner = cpUtf8(cf, cf.cp.at(e.a).a);
        const auto&       nat   = cf.cp.at(e.b);
        const std::string name  = cpUtf8(cf, nat.a);
        const std::string desc  = cpUtf8(cf, nat.b);
        if (e.tag == T::Fieldref) {
            out.fieldKeys.insert(owner + "::" + name);
        } else {
            out.methodKeys.insert(owner + "::" + name + "::" + desc);
        }
    }
    return out;
}

/// @brief 类的形状候选收集（SCAN 本体）。
///
/// indyTargets 是跨类运行时目标集（owner+name）：本类 BSM 盲区只覆盖「类内带
/// BootstrapMethods」的形态，而 Grunt 的 BSM 常在 A 类、findStatic 的目标在
/// B 类——普查看不见这种引用，必须由 indy_fold::scan 真跑 BSM 补上。
/// bsmUnresolved（存在未收敛 BSM）时 synthetic/bridge 候选整档豁免：那是 indy
/// 目标的形状（Grunt 给每个调用点造 public static bridge），目标名是解密产物，
/// CP 里零引用；findStatic 跨类又要求 public，private 候选不在此列。
ClassCand scanClass(ir::ClassSession& s, const std::string& uri,
                    const std::set<std::pair<std::string, std::string>>& indyTargets,
                    bool bsmUnresolved)
{
    ClassCand              cand;
    cand.uri   = uri;
    cand.owner = std::string(s.backing().thisName());

    jbc::core::ClassFile&      cf   = s.backing();
    const ir::ClassUnit&       unit = s.unit();

    // 运行时引用面（CP 普查的三类盲区，命中即跳过成员删除）：
    // 1. BootstrapMethods——BSM 可在运行时经解密字符串 findStatic/findVirtual
    //    本类任意成员（Grunt 形态：目标名是密文，字节码里没有明文引用）；
    // 2. 注解字符串——mixin 框架按注解值里的方法名反射定位（@Redirect）；
    // 3. 跨类 indy 目标——别类 BSM 运行时 findStatic 本类成员（indyTargets）。
    // <clinit> 不受此限：它不可被反射句柄定位，只由 JVM 初始化触发。
    const bool hasBsm = cf.findClassAttr("BootstrapMethods") != nullptr;
    const std::set<std::string> annoNames = annotationStrings(s);

    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        const ir::MethodUnit& mu   = unit.methods[mi];
        const std::string     name = std::string(unit.symbols().str(mu.name));
        const std::string     desc = std::string(unit.symbols().str(mu.desc));

        if (name == "<clinit>") {
            // 拆空形状：全部指令是 NOP/RETURN。
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            bool empty = true;
            for (const ir::InsnId id : cu->dom.order()) {
                const ir::Insn* n = cu->dom.peek(id);
                if (n == nullptr) continue;
                if (n->op == jbc::core::Op::NOP || n->op == jbc::core::Op::RETURN) continue;
                empty = false;
                break;
            }
            if (empty) cand.emptyClinitIndex = mu.backingIndex;
            continue;
        }
        if (name == "main") continue;
        if ((mu.access & (kAccNative | kAccAbstract)) != 0) continue;
        if (!ir::hasCode(s, mi)) continue;
        if (hasBsm) continue;                                // BSM 盲区：整类方法不删
        if (annoNames.count(name) != 0) continue;            // 注解按名引用：不删
        if (indyTargets.count({cand.owner, name}) != 0) continue;  // 跨类 BSM 目标：不删

        const bool gateA =
            (mu.access & kAccStatic) != 0
            && [&] {
                   auto promoted = ir::promoteMethod(s, mi);
                   if (promoted.is_err()) return false;
                   ir::CodeUnit* cu = promoted.unwrap().get();
                   return cu != nullptr && pureArithShape(s, *cu);
               }();
        const bool gateB = (mu.access & (kAccSynthetic | kAccBridge)) != 0;
        const bool gateC = (mu.access & kAccPrivate) != 0;
        if (!gateA && !gateB && !gateC) continue;
        if (bsmUnresolved && gateB && !gateC && !gateA) continue;  // indy 目标形状：宁留

        cand.methods.push_back(MethodCand{mu.backingIndex, name, desc,
                                          gateA ? MethodGate::PureArith
                                                : gateB ? MethodGate::SyntheticBridge
                                                        : MethodGate::Private});
    }

    for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
        const jbc::core::MemberInfo& fld  = cf.fields[fi];
        const std::string            fname = cpUtf8(cf, fld.nameCp);
        const std::string            fdesc = cpUtf8(cf, fld.descCp);
        if (fname == "serialVersionUID" || fname == "serialPersistentFields") continue;
        if (fieldHasConstantValue(s, fld)) continue;
        if (hasBsm) continue;                               // BSM 盲区：整类字段不删
        if (annoNames.count(fname) != 0) continue;          // 注解按名引用：不删
        const bool privateGate = (fld.access & kAccPrivate) != 0;
        const bool orphanArray = (fld.access & kAccStatic) != 0 && fdesc == "[I";
        if (!privateGate && !orphanArray) continue;
        cand.fields.push_back(FieldCand{fi, fname, fdesc, orphanArray && !privateGate});
    }
    return cand;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    // 跨类 indy 目标普查：真跑全容器 BSM（indy_fold 的执行面），解析出的目标
    // 方法按 owner+name 保护。BSM 在 A 类、目标在 B 类的引用静态普查看不见，
    // 不保护就会删掉 BSM 运行时才解出的目标（NoSuchMethodError at findStatic）。
    // BSM 未收敛的站点目标未知，计数进台账（链上 IndyFold 在前会先折掉大半）。
    {
        const indy_fold::ScanResult indyScan = indy_fold::scan(container);
        std::set<std::pair<std::string, std::string>> indyTargets;
        for (const indy_fold::Site& site : indyScan.sites) {
            if (!site.target.owner.empty() && !site.target.name.empty()) {
                indyTargets.insert({site.target.owner, site.target.name});
            }
        }
        result.indyTargets   = indyTargets.size();
        result.indyBsmFailed = indyScan.bsmFailed;
        const bool bsmUnresolved = indyScan.bsmFailed != 0;
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (!session) {
                ++result.classesSkipped;
                return;
            }
            result.classes.push_back(
                scanClass(*session, std::string(entry.uri), indyTargets, bsmUnresolved));
        });
    }
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // (1) 全容器 CP 压缩：引用闭包前提（死条目会污染零引用判定）。压缩失败
    // 的类不做成员删除，也不作为引用方计入——两边都宁留勿删。
    std::set<std::string> compactFailed;
    {
        std::vector<std::pair<std::string, std::vector<ca::u8>>> snapshot;
        forEachClass(container, [&](const ClassEntry& entry) {
            snapshot.emplace_back(std::string(entry.uri), *entry.bytes);
        });
        for (auto& [uri, bytes] : snapshot) {
            auto session = openSession(bytes);
            if (!session || ir::compactConstantPool(*session).is_err()) {
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
            }
        }
    }

    // (2) 活引用普查（压缩后闭包 = 枚举即活引用）。
    std::set<std::string> liveMethods;
    std::set<std::string> liveFields;
    forEachClass(container, [&](const ClassEntry& entry) {
        if (compactFailed.count(std::string(entry.uri)) != 0) return;
        auto session = openSession(*entry.bytes);
        if (!session) return;
        const CpSnap snap = collectCpSnap(*session);
        liveMethods.insert(snap.methodKeys.begin(), snap.methodKeys.end());
        liveFields.insert(snap.fieldKeys.begin(), snap.fieldKeys.end());
    });

    // (3) 逐类删成员。候选定位按名字+描述复核（scan 之后池已重排，backing
    // 下标漂移）。
    std::map<std::string, const ClassCand*> candByUri;
    for (const ClassCand& cand : result.classes) candByUri[cand.uri] = &cand;

    std::vector<std::pair<std::string, std::vector<ca::u8>>> pending;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (compactFailed.count(uri) != 0) return;
        const auto it = candByUri.find(uri);
        if (it == candByUri.end()) return;
        const ClassCand& cand = *it->second;
        if (cand.methods.empty() && cand.fields.empty()
            && cand.emptyClinitIndex == static_cast<ca::usize>(-1)) {
            return;
        }

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession&     s    = *session;
        jbc::core::ClassFile& cf   = s.backing();
        const ir::ClassUnit&  unit = s.unit();
        const std::string     owner(cand.owner);

        ca::usize hitMethods = 0;
        ca::usize hitFields  = 0;
        ca::usize hitClinits = 0;
        std::set<ca::usize> dropMethods;   // cf.methods 下标
        std::set<ca::usize> dropFields;    // cf.fields 下标

        for (const MethodCand& mc : cand.methods) {
            dirty.attempted += 1;
            const std::string key = owner + "::" + mc.name + "::" + mc.desc;
            if (liveMethods.count(key) != 0) {
                ++dirty.rejectedByRef;
                continue;
            }
            // 复核定位（backing 序 + 名字 + 描述三者一致才算同一成员）。
            bool found = false;
            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                const ir::MethodUnit& mu = unit.methods[mi];
                if (mu.backingIndex != mc.backingIndex) continue;
                if (unit.symbols().str(mu.name) == mc.name
                    && unit.symbols().str(mu.desc) == mc.desc) {
                    dropMethods.insert(mu.backingIndex);
                    found = true;
                }
            }
            if (!found) {
                ++dirty.rejectedByRef;
                continue;
            }
            ++hitMethods;
        }

        for (const FieldCand& fc : cand.fields) {
            dirty.attempted += 1;
            const std::string key = owner + "::" + fc.name;
            if (liveFields.count(key) != 0) {
                ++dirty.rejectedByRef;
                continue;
            }
            bool found = false;
            for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
                const jbc::core::MemberInfo& f = cf.fields[fi];
                if (static_cast<ca::usize>(fi) == fc.backingIndex
                    && cpUtf8(cf, f.nameCp) == fc.name
                    && cpUtf8(cf, f.descCp) == fc.desc) {
                    dropFields.insert(fi);
                    found = true;
                }
                // 不 break：字段下标即 fi 本身，唯一命中。
            }
            if (!found) {
                ++dirty.rejectedByRef;
                continue;
            }
            ++hitFields;
        }

        if (cand.emptyClinitIndex != static_cast<ca::usize>(-1)) {
            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                const ir::MethodUnit& mu = unit.methods[mi];
                if (mu.backingIndex == cand.emptyClinitIndex
                    && unit.symbols().str(mu.name) == "<clinit>") {
                    dropMethods.insert(mu.backingIndex);
                    ++hitClinits;
                }
            }
        }

        if (dropMethods.empty() && dropFields.empty()) return;

        for (auto it = dropMethods.rbegin(); it != dropMethods.rend(); ++it) {
            if (*it < cf.methods.size()) {
                cf.methods.erase(cf.methods.begin() + static_cast<ca::i64>(*it));
            }
        }
        for (auto it = dropFields.rbegin(); it != dropFields.rend(); ++it) {
            if (*it < cf.fields.size()) {
                cf.fields.erase(cf.fields.begin() + static_cast<ca::i64>(*it));
            }
        }
        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) return;
        pending.emplace_back(uri, std::move(encoded).unwrap());
        dirty.methods += hitMethods;
        dirty.fields += hitFields;
        dirty.clinits += hitClinits;
    });

    for (auto& [uri, bytes] : pending) {
        replaceClass(container, uri, std::move(bytes));
        dirty.uris.push_back(uri);
    }

    // (4) 删除后重开压缩一次，把被删成员的描述符 utf8 从池里带走；仅压缩
    // 的类也进 proof 面（落过盘就要可重解析）。
    {
        std::vector<std::pair<std::string, std::vector<ca::u8>>> after;
        forEachClass(container, [&](const ClassEntry& entry) {
            after.emplace_back(std::string(entry.uri), *entry.bytes);
        });
        for (auto& [uri, bytes] : after) {
            auto session = openSession(bytes);
            if (!session) continue;
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
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::dead_member
