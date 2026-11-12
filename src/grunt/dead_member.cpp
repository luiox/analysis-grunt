/// @file        dead_member.cpp
/// @brief       残留 `dead_member` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## scan/rewrite 分工
///
/// scan 只产**形状候选**（方法体形状 / 字段访问位 / clinit 拆空）；零引用
/// 判定必须发生在 rewrite，它依赖先压缩再普查这个改写动作，scan 不能
/// 改容器。rewrite 里候选可能被引用检查拒掉（rejectedByRef），这是设计内的。
///
/// ## 压缩的双重身份
///
/// (1) 普查前提，死条目会污染零引用判定；(2) 删除后的收尾，被删成员的
/// 描述符 utf8 从池里带走。两步都落盘；仅压缩变小的类也进 dirty.uris（落
/// 过盘就要过 proof 的可重解析检查）。
///
/// ## 引用键口径与普查分层
///
/// 方法键 owner::name::desc，字段键 owner::name。BootstrapMethods 引用的
/// MethodHandle→Methodref 在压缩闭包内存活，自然计入活引用，indy 引导方
/// 法引用的成员不会被误删。普查**分层兜底**（Kasumi 批修复的教训：普查曾
/// 是压缩成功类才作引用方，压缩失败类被整体排除后其对外引用从活引用
/// 集蒸发，下游类被误删、运行期 NoSuchMethodError，取证见 kasumi-verify
/// §3）：会话可开 → 枚举池；会话打不开 / 普查异常 → 原始字节骨架解析
/// （readClass，只解头/池/成员目录，Raw 面不挡路）再枚举；骨架也解不开 →
/// census-blind，本轮成员删除与空壳回收整体放弃。压缩失败的类仍不作删除
/// 候选（两边宁留），但引用照入普查；普查挂起时间片跑全量（proof 类数检查
/// 同一检查）。同病同修：field_census（ConstValuePromote / SeedStrip 的访问面
/// 普查）补完整性统计、不完整整轮放弃；booger_anno_strip 引用检查对会话打
/// 不开的类按引用在册关死。
///
/// ## ACC_BRIDGE 豁免的核查结论（不改检查）
///
/// ACC_BRIDGE 零引用豁免在 Grunteon 生成面不携带信息：FakeSyntheticBridge
/// 给全体非 abstract 方法补 ACC_BRIDGE|ACC_SYNTHETIC，真方法、indy 目标、
/// 死代理同一形状，真桥排除面会把这类 jar 里几乎所有成员从豁免里摘出去，
/// 恰好打掉豁免要保的对象。豁免的触发条件是 bsmUnresolved（容器存在未收
/// 敛 BSM）：该前提下任何 public synthetic/bridge 成员都无法证明零引用死
/// （BSM 运行时 findStatic 的目标没有 CP 明文引用），误保死代理是设计内的
/// 保守价；indy_fold 收敛后豁免不再触发，死代理走 gateB + 引用普查正常删
/// 除（链序上 indy_fold 恒在本残留之前）。带操作数 xload 的伪 bridge
/// 体形排除对 indy 目标同样成立不了，indy 目标是真方法体普遍带操作数装
/// 载，与死代理形状无法区分；pureArithShape（gateA）已是唯一不进豁免的体
/// 形删除面。
///
/// ## 空壳摘除检查的放行边界
///
/// `<init>`-only + 零字段 + 全容器引用普查 + main 检查是空壳摘除的现行形态；
/// **不新增仅 public 无参构造 + 零字段放行形态**，没有活样本支撑，放
/// 行只会在无证据面扩大摘除权；语料再现此形态时先补 jar 再议。
#include "grunt/dead_member.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/util/annotation_depth.hpp"

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
/// 's' 字符串值、'e' 枚举常量名、'c' 类描述符都收，运行时框架（mixin 等）
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
            // 注解族槽先过嵌套深度预检（mjbase #158）：libjbc attr_codec 注解
            // 解码互递归无界，敌意深嵌注解会在 decodeAttr 内栈溢出；预检拒绝
            // 按解不开原样保真fail-open 跳过，少保一条注解字符串值，
            // 进程不被打爆。
            if (!mjbase::util::attrSafeToDecode(cf, slot)) continue;
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

/// @brief 单类 CP 的成员引用快照：枚举 Fieldref/Methodref/接口引用。
///
/// 压缩成功的类上池是闭包（枚举即活引用）；压缩失败/未压缩的类上池可能含
/// 死条目，枚举结果是活引用的超集，两类都按 fail-closed 宁误留口径入集。
struct CpSnap
{
    std::set<std::string> methodKeys;   // owner::name::desc
    std::set<std::string> fieldKeys;    // owner::name
};

CpSnap collectCpSnap(const jbc::core::ClassFile& cf)
{
    CpSnap out;
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

/// @brief 把快照并入全容器活引用集。
void mergeSnap(const CpSnap& snap, std::set<std::string>& liveMethods,
               std::set<std::string>& liveFields)
{
    liveMethods.insert(snap.methodKeys.begin(), snap.methodKeys.end());
    liveFields.insert(snap.fieldKeys.begin(), snap.fieldKeys.end());
}

/// @brief 类的形状候选收集（SCAN 本体）。
///
/// indyTargets 是跨类运行时目标集（owner+name）：本类 BSM 盲区只覆盖类内带
/// BootstrapMethods的形态，而 Grunt 的 BSM 常在 A 类、findStatic 的目标在
/// B 类，普查看不见这种引用，必须由 indy_fold::scan 真跑 BSM 补上。
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
    // 1. BootstrapMethods，BSM 可在运行时经解密字符串 findStatic/findVirtual
    //    本类任意成员（Grunt 形态：目标名是密文，字节码里没有明文引用）；
    // 2. 注解字符串，mixin 框架按注解值里的方法名反射定位（@Redirect）；
    // 3. 跨类 indy 目标，别类 BSM 运行时 findStatic 本类成员（indyTargets）。
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
    // BSM 未收敛的站点目标未知，计数进统计（链上 IndyFold 在前会先折掉大半）。
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
        static const bool dmTrace = std::getenv("GRUNT_DM_TRACE") != nullptr;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (dmTrace) {
                std::fprintf(stderr, "[grunt.DeadMember] scan class %s\n",
                             std::string(entry.uri).c_str());
            }
            auto session = openSession(*entry.bytes);
            if (!session) {
                ++result.classesSkipped;
                return;
            }
            // 类级 fail-closed：单类形状扫描崩了（libjsim/jbc 抛点或 SEH 硬崩）
            // 只跳过该类，其余类照常出候选。
            try {
                result.classes.push_back(
                    scanClass(*session, std::string(entry.uri), indyTargets, bsmUnresolved));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[grunt.DeadMember] scan 类跳过：%s（异常穿透：%s）\n",
                             std::string(entry.uri).c_str(), e.what());
                ++result.classesSkipped;
            } catch (...) {
                std::fprintf(stderr, "[grunt.DeadMember] scan 类跳过：%s（SEH 硬崩）\n",
                             std::string(entry.uri).c_str());
                ++result.classesSkipped;
            }
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
    static const bool dmTrace = std::getenv("GRUNT_DM_TRACE") != nullptr;

    // (1) 全容器 CP 压缩：引用闭包前提（死条目会污染零引用判定）。压缩失败
    // 的类不做成员删除，也不作为引用方计入，两边都宁留勿删。
    std::set<std::string> compactFailed;
    {
        std::vector<std::pair<std::string, std::vector<ca::u8>>> snapshot;
        forEachClass(container, [&](const ClassEntry& entry) {
            snapshot.emplace_back(std::string(entry.uri), *entry.bytes);
        });
        for (auto& [uri, bytes] : snapshot) {
            // 类级 fail-closed：单类压缩崩了按压缩失败收场（两边都宁留勿删）。
            // 压缩前先过注解族深度预检（classAnnoDepthSafe）：cp_compact 会解
            // 全部属性槽，敌意深嵌注解在 libjbc 互递归解码里栈溢出（不走
            // Result 错误通道），预检拒绝按压缩失败同一通道原样保真。
            try {
                auto session = openSession(bytes);
                if (!session || !classAnnoDepthSafe(session->backing())
                    || ir::compactConstantPool(*session).is_err()) {
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
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[grunt.DeadMember] compact 类跳过：%s（异常穿透：%s）\n",
                             uri.c_str(), e.what());
                compactFailed.insert(uri);
            } catch (...) {
                std::fprintf(stderr, "[grunt.DeadMember] compact 类跳过：%s（SEH 硬崩）\n",
                             uri.c_str());
                compactFailed.insert(uri);
            }
        }
    }

    // 压缩失败类点账（trace 面）：它们不作删除候选，但引用照入下面的普查。
    if (dmTrace) {
        for (const std::string& uri : compactFailed) {
            std::fprintf(stderr,
                         "[grunt.DeadMember] compact 未过封闭集：%s（不作删除候选，引用照入普查）\n",
                         uri.c_str());
        }
    }

    // (2) 活引用普查（压缩后闭包 = 枚举即活引用）。
    //
    // 全容器口径：每个类都必须成为引用方，缺一个类就是漏计（验证案例：压缩
    // 失败类被排除在普查外，`w` 对 `t.aa` 的 Methodref 蒸发，`t` 类 93 方法
    // 全删）。分层收集：
    // - 会话可开 + 普查不崩：枚举池入集（压缩成功类 = 活引用；未压缩类 =
    //   超集，同样宁误留）；
    // - 会话打不开 / 普查崩了：按原始字节骨架解析（readClass 只解头、池、
    //   成员目录，不碰属性体，封闭集之外的 Raw 属性面也不挡路）再枚举原池；
    // - 连骨架都解不开：引用证据完全不可得，任何候选都无法被证明死，置
    //   censusBlind，本轮成员删除整体放弃（零删除，宁整轮不删不冒删错）。
    // 普查是效力检查不是改写面：挂起时间片（proof 类数检查同一检查），不许被超墙
    // 截短出晚到的类不在普查里的漏计。
    std::set<std::string> liveMethods;
    std::set<std::string> liveFields;
    {
        ca::usize censusRawSources = 0;
        ca::usize censusBlind      = 0;
        pass_clock::Suspend suspend;
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (session) {
                try {
                    mergeSnap(collectCpSnap(session->backing()), liveMethods, liveFields);
                    return;
                } catch (...) {
                    std::fprintf(stderr,
                                 "[grunt.DeadMember] 引用普查走原始字节路径：%s（会话普查异常穿透）\n",
                                 std::string(entry.uri).c_str());
                }
            }
            ++censusRawSources;
            auto raw = jbc::core::readClass(jbc::core::ByteView(*entry.bytes),
                                            jbc::core::ReadOptions{});
            if (raw.is_err()) {
                ++censusBlind;
                std::fprintf(stderr,
                             "[grunt.DeadMember] 普查盲区：%s（骨架解析失败，引用证据不可得）\n",
                             std::string(entry.uri).c_str());
                return;
            }
            jbc::core::ReadResult parsed = std::move(raw).unwrap();
            if (parsed.file == nullptr) {
                ++censusBlind;
                std::fprintf(stderr,
                             "[grunt.DeadMember] 普查盲区：%s（骨架模型缺失，引用证据不可得）\n",
                             std::string(entry.uri).c_str());
                return;
            }
            mergeSnap(collectCpSnap(*parsed.file), liveMethods, liveFields);
        });
        dirty.censusRawSources = censusRawSources;
        dirty.censusBlind      = censusBlind;
        if (dmTrace) {
            std::fprintf(stderr,
                         "[grunt.DeadMember] 普查：methodKeys=%zu fieldKeys=%zu"
                         " raw-sources=%zu blind=%zu\n",
                         liveMethods.size(), liveFields.size(), censusRawSources,
                         censusBlind);
        }
    }

    // census 盲区 fail-closed：有类连引用面都枚举不出，全容器任何候选都可能
    // 被它引用，本轮删除整体放弃（压缩已做的类级字节变化保留：压缩不删
    // 成员，引用一致性不受影响）。空壳回收同一检查停：解不开的类可能按名引用壳。
    if (dirty.censusBlind != 0) {
        std::fprintf(stderr,
                     "[grunt.DeadMember] census-blind %zu 类：本轮成员删除与空壳回收整体放弃（宁整轮不删）\n",
                     dirty.censusBlind);
        return dirty;
    }

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

        // 类级 fail-closed：删成员崩了整类放弃（容器保有原字节，本 lambda 的
        // 全部改动都落在局部 session，pending 只在成功末尾追加）。
        try {
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
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[grunt.DeadMember] 删成员类跳过：%s（异常穿透：%s）\n",
                         uri.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr, "[grunt.DeadMember] 删成员类跳过：%s（SEH 硬崩）\n", uri.c_str());
        }
    });

    for (auto& [uri, bytes] : pending) {
        replaceClass(container, uri, std::move(bytes));
        dirty.uris.push_back(uri);
    }

    // (3.5) 空壳类回收：全成员删净且全容器零类级引用的外置壳类整体摘除。
    // 判据保守：删后 methods/fields 全空 ∧ 非 module-info/package-info ∧
    // 其余全类 CP 无 Classref / 同名 utf8 / L<name>; 描述符引用。含 main 的
    // 主类不可能过第一道检查（main 是成员）。经 getResources() 可变视图摘除。
    {
        std::vector<std::string> shellUris;
        std::vector<std::string> shellNames;
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            jbc::core::ClassFile& cf = session->backing();
            // 零存活成员检查：fields 全空；methods 只剩 <init>（真实代理壳带的
            // 默认构造器），外部构造需要 CP Class 引用，引用普查会拦。带
            // main / <clinit> 的类不可能过此检查。
            if (!cf.fields.empty()) return;
            for (const jbc::core::MemberInfo& m : cf.methods) {
                if (cpUtf8(cf, m.nameCp) == "<init>") continue;
                return;
            }
            const std::string name(cf.thisName());
            if (name == "module-info" || name == "package-info") return;
            shellUris.emplace_back(entry.uri);
            shellNames.push_back(name);
        });
        if (!shellUris.empty()) {
            std::vector<bool> shellDead(shellUris.size(), true);
            forEachClass(container, [&](const ClassEntry& entry) {
                const std::string self(entry.uri);
                // 类模型：会话优先，打不开走原始字节骨架解析（引用普查同一
                // 分层，解不开的类不许按零引用计数）；两边都失败按
                // 引用在册处理，空壳摘除是删动词，宁误留勿误删。
                auto                                  session = openSession(*entry.bytes);
                const jbc::core::ClassFile*           model
                    = session ? &session->backing() : nullptr;
                std::unique_ptr<jbc::core::ClassFile> rawModel;
                if (model == nullptr) {
                    auto raw = jbc::core::readClass(jbc::core::ByteView(*entry.bytes),
                                                    jbc::core::ReadOptions{});
                    if (raw.is_err()) {
                        for (ca::usize i = 0; i < shellUris.size(); ++i) {
                            shellDead[i] = false;
                        }
                        return;
                    }
                    rawModel = std::move(std::move(raw).unwrap().file);
                    if (rawModel == nullptr) {
                        for (ca::usize i = 0; i < shellUris.size(); ++i) {
                            shellDead[i] = false;
                        }
                        return;
                    }
                    model = rawModel.get();
                }
                const jbc::core::ClassFile& cf = *model;
                for (ca::usize i = 0; i < shellUris.size(); ++i) {
                    if (!shellDead[i] || shellUris[i] == self) continue;
                    const std::string descNeedle = "L" + shellNames[i] + ";";
                    if (dmTrace) {
                        std::fprintf(stderr, "[grunt.DeadMember] 引用普查 %s → 查 %s\n",
                                     std::string(entry.uri).c_str(), shellNames[i].c_str());
                    }
                    for (ca::u32 ci = 1; ci < cf.cp.size(); ++ci) {
                        if (!cf.cp.valid(ci)) continue;
                        const auto& e = cf.cp.at(ci);
                        using T       = jbc::core::CpTag;
                        if (e.tag == T::Class) {
                            if (cpUtf8(cf, e.a) == shellNames[i]) shellDead[i] = false;
                        } else if (e.tag == T::Utf8) {
                            const std::string s(cpUtf8(cf, ci));
                            if (dmTrace && !s.empty()) {
                                std::fprintf(stderr, "[grunt.DeadMember]   utf8[%u]=%s\n", ci,
                                             s.c_str());
                            }
                            if (s == shellNames[i] || s.find(descNeedle) != std::string::npos) {
                                shellDead[i] = false;
                            }
                        }
                        if (!shellDead[i]) break;
                    }
                }
            });
            auto& resources = container.getResources();
            for (ca::usize i = 0; i < shellUris.size(); ++i) {
                if (!shellDead[i]) continue;
                std::fprintf(stderr, "[grunt.DeadMember] 空壳类摘除：%s\n", shellUris[i].c_str());
                resources.erase(std::string(shellUris[i]));
                // 摘除的类不再进 proof 重解析面（可能刚在本轮删过成员落过盘）。
                for (auto it = dirty.uris.begin(); it != dirty.uris.end();) {
                    if (*it == shellUris[i]) it = dirty.uris.erase(it); else ++it;
                }
                dirty.removedUris.push_back(shellUris[i]);
                ++dirty.classes;
            }
        }
    }

    // (4) 删除后重开压缩一次，把被删成员的描述符 utf8 从池里带走；仅压缩
    // 的类也进 proof 面（落过盘就要可重解析）。
    {
        std::vector<std::pair<std::string, std::vector<ca::u8>>> after;
        forEachClass(container, [&](const ClassEntry& entry) {
            after.emplace_back(std::string(entry.uri), *entry.bytes);
        });
        for (auto& [uri, bytes] : after) {
            // 类级 fail-closed：二次压缩崩了的类保有删成员后的字节，不再压缩。
            try {
                auto session = openSession(bytes);
                if (!session) continue;
                // 二次压缩同一道注解族深度预检：越限类保有删成员后的字节，
                // 不再压缩（与 compact 失败同一原样保真口径）。
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
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[grunt.DeadMember] 二次压缩类跳过：%s（异常穿透：%s）\n",
                             uri.c_str(), e.what());
            } catch (...) {
                std::fprintf(stderr, "[grunt.DeadMember] 二次压缩类跳过：%s（SEH 硬崩）\n",
                             uri.c_str());
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
    // 类数检查：整类摘除（dirty.classes）按预期递减核账，其余场合类数不变；
    // 落盘类必须可重解析，整类摘除的 URI 已不在场，不进重解析面。
    ProofResult      result;
    const ca::usize  expected = classesBefore - dirty.classes;
    const ca::usize  now      = countClasses(container);
    if (now != expected) {
        result.ok = false;
        result.failed.push_back("class-count");
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
    return result;
}

}   // namespace grunt::dead_member
