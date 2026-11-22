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
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/cp_resolve.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/indy_fold.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/pipeline_adapter.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/engine.hpp"
#include "jsim/num.hpp"
#include "jsim/str.hpp"
#include "jsim/value.hpp"

namespace grunt::dead_member {

namespace {

namespace ir = jbc::ir;

constexpr ca::u32 kAccPrivate    = 0x0002;
constexpr ca::u32 kAccStatic     = 0x0008;
constexpr ca::u32 kAccBridge     = 0x0040;
constexpr ca::u32 kAccNative     = 0x0100;
constexpr ca::u32 kAccAbstract   = 0x0400;
constexpr ca::u32 kAccSynthetic  = 0x1000;

/// @brief 解密器族收尾件形状：private static `([CJI)Ljava/lang/String;`。
///
/// ConstantCall 解密器族的目标方法专用描述符（char[]·long·int → String）。
/// 链上 ConstantCall 已把全部填充点折成明文 ldc，此后它是零引用内部件。
/// BSM 盲区与反射按名普查对它豁免（风险论证见 scan hasBsm 块注释）。
bool decryptorTailShape(ca::u32 access, const std::string& desc)
{
    return (access & (kAccStatic | kAccPrivate)) == (kAccStatic | kAccPrivate)
           && desc == "([CJI)Ljava/lang/String;";
}

/// @brief 解密器族第二形描述符面：private static
///        `(Ljava/lang/String;)Ljava/lang/String;`（260705 逐码元 XOR）。
///
/// 描述符本身不是混淆器专有（真实代码也有 private static String 变换助
/// 手），所以它只做预滤；豁免要体形证据（stringXorDecryptorShape）加同类
/// 零调用点普查（decOrphan 走查）双证才成立。
bool stringXorDecryptorDesc(ca::u32 access, const std::string& desc)
{
    return (access & (kAccStatic | kAccPrivate)) == (kAccStatic | kAccPrivate)
           && desc == "(Ljava/lang/String;)Ljava/lang/String;";
}

/// @brief 解密器族第二形体形：逐码元 XOR 环（length 定长 + charAt 取码元
///        + 数值载荷 + IXOR/I2C + StringBuilder.append(C)）+ toString
///        收尾 ARETURN。
///
/// 调用面白名单只收五件（SB.<init>:()V / SB.append:(C) / SB.toString /
/// String.length:()I / String.charAt:(I)C）；体面数值载荷只许数（字符串
/// 混入即非本族）；禁字段/数组访问、其余调用与引用类操作（new 除 SB 前
/// 导）。NOP 容忍：链上 ArithFold 的恒等折叠会留 NOP 缝（240705 键链同
/// 口径），body 里跳过即可。真实代码的 private String 助手若被直接调
/// 用，decOrphan 普查（同类零 INVOKESTATIC）会拦下——本形只在"孤儿 +
/// 体形"双证下进豁免，反射按名调用一个池填充内部件不构成保护面（
/// q8 bA.i/a/b 实证：名字撞 junk 池明文 "a"/"b"/"i"，池读点全是 public
/// junk 方法，与解密器无关）。
bool stringXorDecryptorShape(ir::ClassSession& s, ir::CodeUnit& cu)
{
    using jbc::core::Op;
    bool gotLength   = false;
    bool gotCharAt   = false;
    bool gotAppend   = false;
    bool gotToString = false;
    bool gotIxor     = false;
    for (const ir::InsnId id : cu.dom.order()) {
        const ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        switch (in->op) {
        case Op::NOP:
            continue;
        case Op::LDC:
        case Op::LDC_W: {
            ir::ClassSession::ConstSem sem;
            if (!s.constOf(*in, sem)) return false;
            using K = ir::ClassSession::ConstSem::Kind;
            if (sem.kind != K::Int && sem.kind != K::Long && sem.kind != K::Float
                && sem.kind != K::Double) {
                return false;
            }
            continue;
        }
        case Op::NEW:
            if (s.classOf(*in) != "java/lang/StringBuilder") return false;
            continue;
        case Op::INVOKESPECIAL: {
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*in, sem)) return false;
            if (sem.owner == "java/lang/StringBuilder" && sem.name == "<init>"
                && sem.desc == "()V") {
                continue;
            }
            return false;
        }
        case Op::INVOKEVIRTUAL: {
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*in, sem)) return false;
            if (sem.owner == "java/lang/StringBuilder" && sem.name == "append"
                && sem.desc == "(C)Ljava/lang/StringBuilder;") {
                gotAppend = true;
                continue;
            }
            if (sem.owner == "java/lang/StringBuilder" && sem.name == "toString"
                && sem.desc == "()Ljava/lang/String;") {
                gotToString = true;
                continue;
            }
            if (sem.owner == "java/lang/String" && sem.name == "length"
                && sem.desc == "()I") {
                gotLength = true;
                continue;
            }
            if (sem.owner == "java/lang/String" && sem.name == "charAt"
                && sem.desc == "(I)C") {
                gotCharAt = true;
                continue;
            }
            return false;
        }
        case Op::IXOR:
            gotIxor = true;
            continue;
        case Op::ARETURN:
            continue;
        default: {
            const int op = static_cast<int>(in->op);
            const bool ok = (op >= 0x00 && op <= 0x2d)     // nop/const/locals（ldc 已单独查）
                            || (op >= 0x36 && op <= 0x4e)  // 局部 store 族
                            || (op >= 0x57 && op <= 0x5f)  // 栈操作
                            || (op >= 0x60 && op <= 0x98)  // 算术/转换/比较
                            || (op >= 0x99 && op <= 0xa7)  // if/icmp/goto
                            || op == 0xaa || op == 0xab;   // tableswitch/lookupswitch
            if (!ok) return false;   // 返回族/字段/数组/其余引用操作一律非本族
            continue;
        }
        }
    }
    return gotLength && gotCharAt && gotAppend && gotToString && gotIxor;
}

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

/// @brief 无参 static getter 形状：体恰为 GETSTATIC + 值返回（两条实体指
///        令，NOP 落点不算），无异常表。
///
/// Grunt 的字段代理在 FieldAccessProxy/ProxyInline 折完全部调用点后剩下的
/// 零引用壳（`public static String[] o() { return a; }` 一族，public 不落
/// private 门）。删整个方法没有布局/标签/帧面，形状只限定爆炸半径。
bool pureGetterShape(ir::CodeUnit& cu)
{
    using jbc::core::Op;
    if (!cu.exc.empty()) return false;
    ca::usize n  = 0;
    bool      got = false;
    for (const ir::InsnId id : cu.dom.order()) {
        const ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        if (in->op == Op::NOP) continue;
        ++n;
        if (n == 1) {
            if (in->op != Op::GETSTATIC) return false;
            got = true;
            continue;
        }
        if (n == 2) {
            switch (in->op) {
            case Op::ARETURN:
            case Op::IRETURN:
            case Op::LRETURN:
            case Op::FRETURN:
            case Op::DRETURN: return got;
            default: return false;
            }
        }
        return false;
    }
    return false;   // 少于两条实体指令不成立
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

/// @brief 单类 CP 的成员引用快照：枚举 Fieldref/Methodref/接口引用 + 字符串常量。
///
/// 压缩成功的类上池是闭包（枚举即活引用）；压缩失败/未压缩的类上池可能含
/// 死条目，枚举结果是活引用的超集，两类都按 fail-closed 宁误留口径入集。
/// 字符串常量是反射按名定位（`getDeclaredMethod("name")` 一族）的证据面：
/// 引用只存在于运行期字符串里，静态引用普查看不见，宁误留。
struct CpSnap
{
    std::set<std::string> methodKeys;   // owner::name::desc
    std::set<std::string> fieldKeys;    // owner::name
    std::set<std::string> strings;      // 字符串常量内容
};

CpSnap collectCpSnap(const jbc::core::ClassFile& cf)
{
    CpSnap out;
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (!cf.cp.valid(i)) continue;
        const auto& e = cf.cp.at(i);
        using T       = jbc::core::CpTag;
        if (e.tag == T::String) {
            if (cf.cp.valid(e.a)) out.strings.insert(cpUtf8(cf, e.a));
            continue;
        }
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
               std::set<std::string>& liveFields, std::set<std::string>& liveNames)
{
    liveMethods.insert(snap.methodKeys.begin(), snap.methodKeys.end());
    liveFields.insert(snap.fieldKeys.begin(), snap.fieldKeys.end());
    liveNames.insert(snap.strings.begin(), snap.strings.end());
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
                    const std::set<ca::u32>& indyUnresolvedSlots, bool bsmUnresolved,
                    const std::set<std::string>& containerClasses)
{
    ClassCand              cand;
    cand.uri   = uri;
    cand.owner = std::string(s.backing().thisName());

    jbc::core::ClassFile&      cf   = s.backing();
    const ir::ClassUnit&       unit = s.unit();

    // 活 BSM 条目（CP InvokeDynamic 引用的下标集）与它们指向的 BSM 方法名。
    //indy 面收链后死条目同轮被属性剪除（rewrite 压缩段），孤儿 BSM 方法的
    // gateE 与盲区收窄都以「活条目」为准。
    std::set<ca::u32>       liveBsmIdx;
    std::set<std::string>   liveBsmTargetNames;
    bool                    liveInternalEntry = false;   // 活条目指向容器内 BSM
    {
        const jbc::core::ConstantPool& cp = cf.cp;
        for (ca::usize i = 1; i < cp.size(); ++i) {
            if (!cp.valid(i)) continue;
            const jbc::core::ConstantPool::Entry& e = cp.at(i);
            if (e.tag == jbc::core::CpTag::InvokeDynamic && e.a < 0x10000) {
                liveBsmIdx.insert(e.a);
            }
        }
        const auto* bsms = scan_tools::bsmEntries(s);
        if (bsms != nullptr) {
            for (ca::u32 idx : liveBsmIdx) {
                if (idx >= bsms->size()) continue;
                jbc::core::HandleSem hsem;
                if (!cp.valid((*bsms)[idx].methodRef)) continue;
                if (!jbc::core::resolveHandleCp(cp, (*bsms)[idx].methodRef, hsem)) continue;
                liveBsmTargetNames.insert(std::string(hsem.name));
                if (containerClasses.count(std::string(hsem.owner)) != 0) {
                    liveInternalEntry = true;
                }
            }
        } else if (!liveBsmIdx.empty()) {
            liveInternalEntry = true;   // 条目表解不开：宁按盲区处理
        }
    }

    // 运行时引用面（CP 普查的三类盲区，命中即跳过成员删除）：
    // 1. BootstrapMethods，BSM 可在运行时经解密字符串 findStatic/findVirtual
    //    本类任意成员（Grunt 形态：目标名是密文，字节码里没有明文引用）——
    //    盲区以**活条目指向容器内 BSM** 为准：indy 面收链后活条目只剩 JDK
    //    metafactory 族（目标静态可见、已入 indyTargets），死条目同轮剪除，
    //    两者都不构成盲区（2026-11-17 收窄，q8/t3 孤儿 BSM 回收实证）；
    // 2. 注解字符串，mixin 框架按注解值里的方法名反射定位（@Redirect）；
    // 3. 跨类 indy 目标，别类 BSM 运行时 findStatic 本类成员（indyTargets）。
    // <clinit> 不受此限：它不可被反射句柄定位，只由 JVM 初始化触发。
    const bool hasBsm   = cf.findClassAttr("BootstrapMethods") != nullptr;
    const bool bsmBlind = hasBsm && liveInternalEntry;
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
        // 构造器反射可达（getDeclaredConstructor + setAccessible，按类型定位，
        // 无名字证据可用）：整档不删，宁误留。
        if (name == "<init>") continue;
        if ((mu.access & (kAccNative | kAccAbstract)) != 0) continue;
        if (!ir::hasCode(s, mi)) continue;
        // gateE 前置判定：BSM 签名孤儿（Grunt simple/heavy 签名 + 无活条目指
        // 向）。JVM 对 BSM 方法的唯一回调通道是 BootstrapMethods 条目；死条目
        // 同轮被 rewrite 压缩段剪除，活条目没指向 = 永不可达，静态普查零引用
        // 即死（indy 面收链后的 BSM 方法与专属 helper 回收，2026-11-17 收）。
        const bool bsmOrphan =
            (desc == knowledge::kGruntSimpleBsmDesc || desc == knowledge::kGruntHeavyBsmDesc)
            && liveBsmTargetNames.count(name) == 0;
        // 解密器第二形体形（描述符预滤 + 体形五件套 + 同类零调用点走查），
        // bsmBlind 例外与 decTail 普查豁免共用一份证据。
        const bool strDecTail =
            stringXorDecryptorDesc(mu.access, desc)
            && [&] {
                   auto promoted = ir::promoteMethod(s, mi);
                   if (promoted.is_err()) return false;
                   ir::CodeUnit* cu = promoted.unwrap().get();
                   return cu != nullptr && stringXorDecryptorShape(s, *cu);
               }();
        if (bsmBlind) {
            // BSM 盲区（活条目指向容器内 BSM，运行时经解密字符串 findStatic
            // 本类成员，静态引用普查看不见）：整类方法不删。例外三支——解密
            // 器族收尾门：特征描述符 `([CJI)Ljava/lang/String;` + private +
            // static 是 ConstantCall 解密器族的专用件（全部填充点已折成明文
            // ldc，零明文引用）；盲区风险被三重证据压住：private 不可跨类直
            // 调、indy 池条目直指它的情形由 indyTargets 普查拦（下一条）、
            // 反射按名命中由 liveNames 普查拦。BSM 运行时反射解密器没有混淆
            // 价值——解密器本身是混淆器生成的池填充内部件，反射调它不构成
            // 保护面。第二形（(String;)String 逐码元 XOR）同口径入例外：
            // 描述符不专有，但体形证据（stringXorDecryptorShape）加 decOrphan
            // 同类零调用点走查双证压住。private+static 落出本块后照常被
            // gateC 收进候选。BSM 签名孤儿同例外：回调通道（条目指向）已
            // 不存在。
            if (!decryptorTailShape(mu.access, desc) && !strDecTail && !bsmOrphan) {
                continue;
            }
        }
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
        // gateB 只认 static 上的 bridge/synthetic 位：Grunteon 260705 给全部
        // 成员撒位（语料实证 t3_plain C/m 实例方法 acc=0x1041，Test 2.1 计数
        // 反射实证破行），实例成员的位在该家族零信息量——javac 的实例
        // synthetic 不是桥（有真调用点，census 保）就是外类引用件（不可
        // 删）；两代混淆器与 javac 的可删静态合成转发器（access$000 /
        // lambda$ / 内联后零引用的 forwarder）全落 static 面。
        const bool gateB = (mu.access & (kAccSynthetic | kAccBridge)) != 0
                           && (mu.access & kAccStatic) != 0;
        const bool gateC = (mu.access & kAccPrivate) != 0;
        const bool gateD =
            (mu.access & kAccStatic) != 0 && desc.size() >= 2 && desc[0] == '('
            && desc[1] == ')'
            && [&] {
                   auto promoted = ir::promoteMethod(s, mi);
                   if (promoted.is_err()) return false;
                   ir::CodeUnit* cu = promoted.unwrap().get();
                   return cu != nullptr && pureGetterShape(*cu);
               }();
        const bool gateE = bsmOrphan;
        if (!gateA && !gateB && !gateC && !gateD && !gateE) continue;
        // bsmUnresolved（存在未收敛 BSM）时所有 static 门整档豁免：Grunteon
        // ReferenceObfuscate 的 indy 目标名是密文（运行时 findStatic 才解出
        // 来），CP 普查看不见引用；被隐藏的目标不限于 synthetic/bridge——
        // RuntimeMaterial 的 processor / GateProcessorFold 的转发体都是
        // public static 纯算术形（gateA），常量 getter 也是 findStatic 命中
        // 面。验收阵 t3_full 实证：豁免只盖 gateB 时 DeadMember 单跑删 219
        // 法，Test 1.1 起 NoSuchMethodError；扩到 gateA/gateD 后整链行为回
        // 稳（2026-11-16 修）。private（gateC）不受影响：findStatic 跨类
        // 不可达，同类 BSM 由 hasBsm 整类豁免覆盖。
        if (bsmUnresolved && !gateC) continue;
        // getter 门按未收敛站点的实参槽形豁免：槽数不吻合的 lambda 站点
        // asType 同 arity 硬规则下不可能命中，不整档宁留。（bsmUnresolved
        // 已整档宁留，本条仅剩无未收敛 BSM 时的余量面。）
        if (bsmUnresolved && gateD && !gateC
            && indyUnresolvedSlots.count(const_args::argSlots(desc)) != 0) {
            continue;
        }

        // 解密器收尾件的普查豁免前提：同类零调用点。ConstantCall 已把常量
        // 形态填充点折净，此处逐方法普查 INVOKESTATIC 复核——非零说明还有
        // 折不动的真调用（t/u 族：key 是 parseUnsignedLong 表达式形态），
        // 普查保护是唯一防线，不豁免。
        const bool decTail  = decryptorTailShape(mu.access, desc) || strDecTail;
        bool      decOrphan = decTail;
        if (decTail) {
            for (ca::usize wi = 0; wi < unit.methods.size() && decOrphan; ++wi) {
                if (!ir::hasCode(s, wi)) continue;
                auto walked = ir::promoteMethod(s, wi);
                if (walked.is_err()) continue;
                ir::CodeUnit* wcu = walked.unwrap().get();
                if (wcu == nullptr) continue;
                for (const ir::InsnId id : wcu->dom.order()) {
                    const ir::Insn* in = wcu->dom.peek(id);
                    if (in == nullptr || in->op != jbc::core::Op::INVOKESTATIC) continue;
                    ir::ClassSession::MemberSem sem;
                    if (!s.memberOf(*in, sem)) continue;
                    if (sem.owner == cand.owner && sem.name == name && sem.desc == desc) {
                        decOrphan = false;
                        break;
                    }
                }
            }
        }

        cand.methods.push_back(MethodCand{mu.backingIndex, name, desc,
                                          gateA   ? MethodGate::PureArith
                                          : gateB ? MethodGate::SyntheticBridge
                                          : gateC ? MethodGate::Private
                                          : gateD ? MethodGate::Getter
                                                  : MethodGate::BsmOrphan,
                                          decTail, decOrphan});
    }

    for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
        const jbc::core::MemberInfo& fld  = cf.fields[fi];
        const std::string            fname = cpUtf8(cf, fld.nameCp);
        const std::string            fdesc = cpUtf8(cf, fld.descCp);
        if (fname == "serialVersionUID" || fname == "serialPersistentFields") continue;
        if (fieldHasConstantValue(s, fld)) continue;
        if (bsmBlind) continue;                             // BSM 盲区：整类字段不删
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
    // 未收敛站点仍约束目标的形状：BSM 的 findStatic(...).asType(调用点类型)
    // 要求同实参槽数（MethodHandle.asType 同 arity 硬规则），所以只有实参槽
    // 数与未收敛站点调用点描述符吻合的公开候选需要宁留；无参 getter 与
    // `(Ln;)Runnable` 形 lambda 站点（1 槽）形状不可能命中，不必整档豁免。
    {
        // 接口集必须转发：默认空集会让 indy_fold 的无旁证告警在本普查里
        // 误发（15 样本矩阵实测 demo_a_v3 801 条，owner 全是容器类/JDK 类，
        // 语义无损但噪声淹没真信号）；真正的 IndyFold pass 是满集零告警。
        const indy_fold::ScanResult indyScan
            = indy_fold::scan(container, iter_budget::ClassBudget{indy_fold::kScanClassBudget},
                              jdkInterfacesCached());
        std::set<std::pair<std::string, std::string>> indyTargets;
        std::set<ca::u32> indyUnresolvedSlots;
        for (const indy_fold::Site& site : indyScan.sites) {
            if (!site.target.owner.empty() && !site.target.name.empty()) {
                indyTargets.insert({site.target.owner, site.target.name});
                continue;
            }
            if (!site.indyDesc.empty()) {
                indyUnresolvedSlots.insert(const_args::argSlots(site.indyDesc));
            }
        }
        result.indyTargets   = indyTargets.size();
        result.indyBsmFailed = indyScan.bsmFailed;
        const bool bsmUnresolved = indyScan.bsmFailed != 0;
        // 容器类名集（活条目 owner 判内/外用；metafactory 族在外 = 非盲区）。
        std::set<std::string> containerClasses;
        forEachClass(container, [&](const ClassEntry& ce) {
            std::string u(ce.uri);
            if (u.size() > 6 && u.compare(u.size() - 6, 6, ".class") == 0) {
                containerClasses.insert(u.substr(0, u.size() - 6));
            }
        });
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
                    scanClass(*session, std::string(entry.uri), indyTargets,
                              indyUnresolvedSlots, bsmUnresolved, containerClasses));
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

/// @brief 池明文名字普查：模拟执行持有 static String[] 字段的类的 <clinit>，
///        把会话静态区里池数组的元素明文并入 liveNames。
///
/// fork 池形的字符串池数组（clinit 前导 + junk 填充方法）元素是运行期解密
/// 产物：反射按名定位（getDeclaredMethod("add") 一族）的目标名字不在任何
/// CP 字符串常量里，CP 字符串普查看不见。主链 indy 折叠后 clinit 已无
/// indy，模拟器可整段跑通（常量假谓词具体求值、junk 调用走编排层接力、
/// 解密器走 str 扩展）。失败即失败：Bail 的类不入集（少了它的名字，成员
/// 删除多一分风险，但证据面不造假）；含 indy/athrow 的 clinit 在桥面整方法
/// 拒绝，同此口径。
///
/// 死池豁免：池数组的 GETSTATIC 读点全部落在删除候选方法里的池是死池——
/// 名字没有任何候选之外的代码能读出去，反射面够不着，明文不进证据面
/// （junk 池类形态：池 + getter 全零引用，明文是诱饵；样本取证 B.a 只被
/// 候选 B.c/B.e 读）。候选读点存活（ref-hit 被引用拦下）时名字实际可能被
/// 引用方带出去——本判定不做存活推演，与 eval-bail 池同一不造假口径记账；
/// 存活读点把名字带到反射面的形态靠整链重复跑不收敛暴露。
/// 读点普查只在 CP 含该池 Fieldref 的类里做 promote 级走查（池类 CP 预滤，
/// 万类 jar 上不为普查全量 promote）。
void collectPoolNames(const mjbase::model::ResourceContainer& container,
                      const std::map<std::string, const ClassCand*>& candByUri,
                      std::set<std::string>& liveNames)
{
    static const bool dmTrace = std::getenv("GRUNT_DM_TRACE") != nullptr;
    SimExecutor exec(container);
    jsim::str::install(exec.session());
    jsim::str::installStringAlloc(exec.session());   // 池解密器体 `new String(char[])`
    jsim::num::install(exec.session());

    // 池求值 mock（parseUnsignedLong / toCharArray）单源安装（scan_toolbox）。
    scan_tools::installPoolEvalMocks(exec.session());

    static constexpr ca::u32 kAccStatic = 0x0008;

    // 候选类清单：持有 static String[] 字段的类（池数组形态）。
    struct PoolClass
    {
        std::string owner;
        /// 池数组的 (name, desc) 与会话静态区键 owner::name::desc。
        std::vector<std::pair<std::string, std::string>> arrays;
        std::vector<std::string> staticKeys;
        bool live      = false;   ///< 有候选之外的 GETSTATIC 读点
        bool extracted = false;
    };
    std::vector<PoolClass> pools;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        jbc::core::ClassFile& cf = session->backing();
        PoolClass             pc;
        pc.owner = std::string(cf.thisName());
        for (const jbc::core::MemberInfo& f : cf.fields) {
            if ((f.access & kAccStatic) == 0) continue;
            const std::string desc(cf.utf8(f.descCp));
            if (desc != "[Ljava/lang/String;") continue;
            const std::string name(cf.utf8(f.nameCp));
            pc.arrays.emplace_back(name, desc);
            pc.staticKeys.push_back(pc.owner + "::" + name + "::" + desc);
        }
        if (!pc.arrays.empty()) pools.push_back(std::move(pc));
    });
    if (pools.empty()) return;

    // 死池判定：全容器找池数组的 GETSTATIC 读点（CP 预滤出引用池字段的类，
    // promote 级归属到方法），读点方法不是删除候选 → 活池。候选归属按
    // 名字+描述复核（scan 之后池已重排，backing 下标漂移，与删除循环同一
    // 定位口径）。
    {
        std::set<std::string> poolFieldRefs;   // "owner::name::desc"（GETSTATIC 面）
        for (const PoolClass& pc : pools) {
            for (ca::usize i = 0; i < pc.arrays.size(); ++i) {
                poolFieldRefs.insert(pc.staticKeys[i]);
            }
        }
        const auto isCandidate = [&](const std::string& uri, std::string_view name,
                                     std::string_view desc) {
            const auto it = candByUri.find(uri);
            if (it == candByUri.end()) return false;
            for (const MethodCand& mc : it->second->methods) {
                if (mc.name == name && mc.desc == desc) return true;
            }
            return false;
        };
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (!session) return;   // 会话打不开：读点不可知，宁当活池（不豁免）
            const std::string      uri(entry.uri);
            jbc::core::ClassFile&  cf    = session->backing();
            const ir::ClassUnit&   unit = session->unit();

            // CP 预滤：本类没有引用任何池数组字段就整类跳过。
            bool refsPool = false;
            for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
                if (!cf.cp.valid(i)) continue;
                const auto& e = cf.cp.at(i);
                if (e.tag != jbc::core::CpTag::Fieldref) continue;
                const jbc::core::ConstantPool::Entry& nat = cf.cp.at(e.b);
                if (nat.tag != jbc::core::CpTag::NameAndType) continue;
                const std::string owner(cf.utf8(cf.cp.at(e.a).a));
                const std::string name(cf.utf8(nat.a));
                const std::string desc(cf.utf8(nat.b));
                if (poolFieldRefs.count(owner + "::" + name + "::" + desc) != 0) {
                    refsPool = true;
                    break;
                }
            }
            if (!refsPool) return;

            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                if (!ir::hasCode(*session, mi)) continue;
                auto promoted = ir::promoteMethod(*session, mi);
                if (promoted.is_err() || promoted.unwrap().get() == nullptr) {
                    // 读点不可知（promote 拒绝）：宁当活池，明文照进证据面
                    // （与 eval-bail 同一不造假口径；上游写回链缺陷留下的
                    // 坏帧方法在这里 fail-open 会把真反射名弄丢）。
                    for (PoolClass& pc : pools) pc.live = true;
                    return;
                }
                ir::CodeUnit* cu = promoted.unwrap().get();
                const std::string_view mname = unit.symbols().str(unit.methods[mi].name);
                const std::string_view mdesc = unit.symbols().str(unit.methods[mi].desc);
                if (isCandidate(uri, mname, mdesc)) continue;   // 候选读点不算活读点

                for (const ir::InsnId id : cu->dom.order()) {
                    const ir::Insn* in = cu->dom.peek(id);
                    if (in == nullptr || in->op != jbc::core::Op::GETSTATIC) continue;
                    ir::ClassSession::MemberSem sem;
                    if (!session->memberOf(*in, sem)
                        || sem.kind != ir::ClassSession::MemberSem::Kind::Field) {
                        continue;
                    }
                    const std::string key(std::string(sem.owner) + "::" + std::string(sem.name)
                                          + "::" + std::string(sem.desc));
                    if (poolFieldRefs.count(key) == 0) continue;
                    for (PoolClass& pc : pools) {
                        if (pc.owner == sem.owner) pc.live = true;
                    }
                }
            }
        });
        if (dmTrace) {
            for (const PoolClass& pc : pools) {
                if (!pc.live) {
                    std::fprintf(stderr, "[grunt.DeadMember]   dead-pool: %s（%zu 数组）\n",
                                 pc.owner.c_str(), pc.arrays.size());
                }
            }
        }
    }

    // 多轮求值：会话静态区跨类累积（PUTSTATIC 接力棒），依赖别的类 clinit
    // 产物的填充器第一轮读 Unknown 会 Bail，上游类跑完后下一轮收敛。
    // 轮数封顶防环；单类成功即提取，不成功的类不入集（不猜）。
    const std::vector<jsim::SimValue> noArgs;
    for (ca::u32 round = 0; round < 3; ++round) {
        bool progressed = false;
        for (PoolClass& pc : pools) {
            if (pc.extracted || !pc.live) continue;
            const SimOutcome out = exec.invoke(pc.owner, "<clinit>()V", noArgs);
            if (out.kind != SimOutcome::Kind::Void
                && out.kind != SimOutcome::Kind::Returned) {
                continue;   /// 求值未收敛：等下一轮（上游 clinit 可能还没跑）。
            }
            ca::usize got = 0;
            for (const std::string& key : pc.staticKeys) {
                const auto it = exec.session().statics.find(key);
                if (it == exec.session().statics.end()) continue;
                const jsim::SimValue& v = it->second;
                if (!v.known || v.type != jsim::ValType::Ref
                    || v.ref.kind != jsim::RefPayload::Kind::Obj) {
                    continue;
                }
                const auto hit = exec.session().heap.find(v.ref.objId);
                if (hit == exec.session().heap.end()) continue;
                for (const jsim::SimValue& elem : hit->second.refs) {
                    if (!elem.known || elem.type != jsim::ValType::Ref) continue;
                    if (elem.ref.kind == jsim::RefPayload::Kind::Str) {
                        if (dmTrace && elem.ref.str.size() <= 2) {
                            std::fprintf(stderr,
                                         "[grunt.DeadMember]   pool-name(%s): %s\n",
                                         pc.owner.c_str(), elem.ref.str.c_str());
                        }
                        liveNames.insert(elem.ref.str);
                        ++got;
                        continue;
                    }
                    // `new String(char[])` 产物是 Obj 载荷（data 槽装明文），
                    // 与 knownStr 的 Str 载荷是同一语义的两种落地形态。
                    if (elem.ref.kind == jsim::RefPayload::Kind::Obj) {
                        const auto objIt = exec.session().heap.find(elem.ref.objId);
                        if (objIt == exec.session().heap.end()) continue;
                        if (objIt->second.className != "java/lang/String") continue;
                        if (dmTrace && objIt->second.data.size() <= 2) {
                            std::fprintf(stderr,
                                         "[grunt.DeadMember]   pool-name(%s): %s\n",
                                         pc.owner.c_str(), objIt->second.data.c_str());
                        }
                        liveNames.insert(objIt->second.data);
                        ++got;
                    }
                }
            }
            pc.extracted = true;
            progressed   = true;
        }
        if (!progressed) break;
    }
}


// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// @brief 死 BSM 条目剪除（单类，纯写）：活条目 = CP InvokeDynamic 引用的
///        下标；死条目 = indy 面收链后无引用的遗留（JVM 只从条目回调 BSM，
///        剪除不可观察）。剪除后 InvokeDynamic 条目的 bootstrap 下标重映射，
///        全空则整属性摘除（hasBsm 盲区随之解除，孤儿 BSM 方法由 gateE
///        收走）。条目表解不开 / 属性不在场 = 原样返回（宁留勿删）。
void pruneDeadBsmEntries(ir::ClassSession& s)
{
    jbc::core::ClassFile& cf = s.backing();
    jbc::core::AttrSlot*  slot = cf.findClassAttr("BootstrapMethods");
    if (slot == nullptr) return;
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) return;
    auto* body = decoded.unwrap();
    if (!ca::core::tag_cast::isa<jbc::core::BootstrapMethodsAttr>(body)) return;
    auto* attr = ca::core::tag_cast::cast<jbc::core::BootstrapMethodsAttr>(body);
    jbc::core::ConstantPool& cp = cf.cp;
    std::set<ca::u32> liveIdx;
    for (ca::usize i = 1; i < cp.size(); ++i) {
        if (!cp.valid(i)) continue;
        const jbc::core::ConstantPool::Entry& e = cp.at(i);
        if (e.tag == jbc::core::CpTag::InvokeDynamic && e.a < attr->entries.size()) {
            liveIdx.insert(e.a);
        }
    }
    if (liveIdx.size() >= attr->entries.size()) return;
    std::map<ca::u32, ca::u32> remap;
    auto*                      bsm = ca::core::tag_cast::cast<jbc::core::BootstrapMethodsAttr>(
        slot->mutate());
    if (bsm == nullptr) return;
    std::vector<jbc::core::BootstrapMethodsAttr::Entry> kept;
    for (ca::u32 k = 0; k < bsm->entries.size(); ++k) {
        if (liveIdx.count(k) != 0) {
            remap.emplace(k, static_cast<ca::u32>(kept.size()));
            kept.push_back(std::move(bsm->entries[k]));
        }
    }
    bsm->entries = std::move(kept);
    // bootstrap 下标重映射必须走 mutateAt（置脏）：可变 at() 改字段不置脏，
    // writeClass 对未脏槽 copy-through 源字节——模型里已映射的 e.a 静默还原
    // 成旧值（15 样本矩阵实锤：68 个旧值 <78 碰巧合法、10 个 ≥78 触发
    // ClassFileSanitizeGate 越界；demo_a_v3 round-2 挡死）。
    for (ca::usize i = 1; i < cp.size(); ++i) {
        if (!cp.valid(i)) continue;
        if (cp.tagOf(i) != jbc::core::CpTag::InvokeDynamic) continue;
        jbc::core::ConstantPool::Entry& e = cp.mutateAt(i);
        const auto hit = remap.find(e.a);
        if (hit != remap.end()) e.a = hit->second;
    }
    if (bsm->entries.empty()) {
        auto it = std::find_if(cf.attrs.begin(), cf.attrs.end(),
                               [slot](const jbc::core::AttrSlot& a) { return &a == slot; });
        if (it != cf.attrs.end()) cf.attrs.erase(it);
    }
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    static const bool dmTrace = std::getenv("GRUNT_DM_TRACE") != nullptr;

    // 候选索引（死池判定要用，先建）：候选定位按名字+描述复核（scan 之后池
    // 已重排，backing 下标漂移）。
    std::map<std::string, const ClassCand*> candByUri;
    for (const ClassCand& cand : result.classes) candByUri[cand.uri] = &cand;

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
                if (!session) {
                    if (dmTrace) std::fprintf(stderr, "[compact-fail] %s 会话打不开\n", uri.c_str());
                    compactFailed.insert(uri);
                    continue;
                }
                if (!classAnnoDepthSafe(session->backing())) {
                    if (dmTrace) std::fprintf(stderr, "[compact-fail] %s 注解深度预检拒\n", uri.c_str());
                    compactFailed.insert(uri);
                    continue;
                }
                const auto compacted = ir::compactConstantPool(*session);
                if (compacted.is_err()) {
                    if (dmTrace) {
                        std::fprintf(stderr, "[compact-fail] %s 压缩错误=%s attr=%.*s\n",
                                     uri.c_str(),
                                     jbc::ir::to_string(compacted.unwrap_err().code),
                                     static_cast<int>(compacted.unwrap_err().attrName.size()),
                                     compacted.unwrap_err().attrName.data());
                    }
                    compactFailed.insert(uri);
                    continue;
                }
                // 死 BSM 条目剪除：活条目 = 压缩后 CP 里 InvokeDynamic 引用的
                // 下标（indy 面收链后死条目 = 无 indy 引用的遗留；JVM 只从条
                // 目回调 BSM，剪除不可观察）。剪除后 InvokeDynamic 条目的
                // bootstrap 下标重映射；全空则整属性摘除（hasBsm 盲区随之解
                // 除，孤儿 BSM 方法下一轮由 gateE 收走）。
                pruneDeadBsmEntries(*session);
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
    std::set<std::string> liveNames;   // 字符串常量全集（反射按名定位的证据面）
    {
        ca::usize censusRawSources = 0;
        ca::usize censusBlind      = 0;
        pass_clock::Suspend suspend;
        forEachClass(container, [&](const ClassEntry& entry) {
            auto session = openSession(*entry.bytes);
            if (session) {
                try {
                    mergeSnap(collectCpSnap(session->backing()), liveMethods, liveFields,
                              liveNames);
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
            mergeSnap(collectCpSnap(*parsed.file), liveMethods, liveFields, liveNames);
        });
        dirty.censusRawSources = censusRawSources;
        dirty.censusBlind      = censusBlind;
        // 池明文名字普查：模拟执行持有 String[] 池数组的类的 <clinit>，把
        // 运行期解密产物（反射按名定位的目标）并入 liveNames。CP 字符串
        // 常量普查看不见这类名字（fork 池形：名字是运行期解密产物）。死池
        // （读点全落删除候选）的明文不进证据面。
        collectPoolNames(container, candByUri, liveNames);
        if (dmTrace) {
            std::fprintf(stderr,
                         "[grunt.DeadMember] 普查：methodKeys=%zu fieldKeys=%zu"
                         " nameStrings=%zu raw-sources=%zu blind=%zu\n",
                         liveMethods.size(), liveFields.size(), liveNames.size(),
                         censusRawSources, censusBlind);
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

    // (3) 逐类删成员（候选索引 candByUri 已在压缩前建好）。
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
            // 反射按名定位（getDeclaredMethod("name") 一族）：引用只存在于
            // 运行期字符串里，静态引用普查看不见。名字命中容器内任何字符串
            // 常量的成员宁误留。解密器孤儿件（shape + 同类零调用点双证）
            // 豁免：单字符名（如 "a"）撞上 junk 载荷字符串的概率极高，而反
            // 射调一个需要 (char[], long, int) 密钥材料的混淆器内部件不构成
            // 保护面（全量论证见 scan hasBsm 块）。
            if (liveNames.count(mc.name) != 0 && !mc.decryptorOrphan) {
                if (dmTrace) {
                    std::fprintf(stderr, "[grunt.DeadMember]   name-hit: %s.%s %s\n",
                                 owner.c_str(), mc.name.c_str(), mc.desc.c_str());
                }
                ++dirty.rejectedByRef;
                continue;
            }
            const std::string key = owner + "::" + mc.name + "::" + mc.desc;
            if (liveMethods.count(key) != 0 && !mc.decryptorOrphan) {
                if (dmTrace) {
                    std::fprintf(stderr, "[grunt.DeadMember]   ref-hit: %s\n", key.c_str());
                }
                ++dirty.rejectedByRef;
                continue;
            }
            if (mc.decryptorOrphan
                && (liveNames.count(mc.name) != 0 || liveMethods.count(key) != 0)) {
                if (dmTrace) {
                    std::fprintf(stderr, "[grunt.DeadMember]   orphan-exempt: %s %s\n",
                                 owner.c_str(), mc.name.c_str());
                }
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
            // 反射按名定位（getDeclaredField("name") 一族）：宁误留。
            if (liveNames.count(fc.name) != 0) {
                ++dirty.rejectedByRef;
                continue;
            }
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
            // thisName 是全限定内部名（org/demo/package-info 形），裸名精确
            // 比对永不命中——按简单名判。4e16046 语料实测：jsoup 7 个
            // package-info 全被空壳回收误摘，jspecify 注解类（仅被
            // package-info 注解 CP 引用）随后失引用被 converge 传递性摘除。
            const ca::usize slash     = name.rfind('/');
            const std::string simpleName
                = (slash == std::string::npos) ? name : name.substr(slash + 1);
            if (simpleName == "module-info" || simpleName == "package-info") return;
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
