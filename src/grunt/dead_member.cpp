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

#include "grunt/container.hpp"
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
ClassCand scanClass(ir::ClassSession& s, const std::string& uri)
{
    ClassCand              cand;
    cand.uri   = uri;
    cand.owner = std::string(s.backing().thisName());

    jbc::core::ClassFile&      cf   = s.backing();
    const ir::ClassUnit&       unit = s.unit();

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
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        result.classes.push_back(scanClass(*session, std::string(entry.uri)));
    });
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
