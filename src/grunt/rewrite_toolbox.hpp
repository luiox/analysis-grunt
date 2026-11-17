/// @file        rewrite_toolbox.hpp
/// @brief       REWRITE 工具箱：跨残留共享的改写原语。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口三组原语：
/// 按求值结果造一条常量装载（constant_call / const_method 改写段共用）、
/// 单 promote + mjbase 帧链写回（predicate_fold / dead_code /
/// const_value_promote / seed_strip 的改写驱动共用）与资源删除（资源
/// 面改写动词，metadata_strip 首用；核对面在 proof_toolbox）。
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"

#include "jbc/analysis/frame.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/insn/insn_factory.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "jsim/value.hpp"

namespace grunt::rewrite_tools {

template <class To, class From>
To bitCast(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bitCast size mismatch");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

/// @brief goto/goto_w 的目标是否即 backing 布局顺序后继（冗余蹦床判据）。
///
/// 按 **backing 布局真值**（落点 srcPc == 自身 pc + 指令宽：goto 3 字节 /
/// goto_w 5 字节）判，不按 dom.order() 相邻性——order 不保证 pc 升序（异
/// 常 handler 等重排），相邻判据在真 jar 上静默漏站点（b.main 族蹦床
/// TrampolineSqueeze 实测全漏，2026-11-16 修）。
inline bool isFallthroughGoto(const jbc::ir::CodeUnit& cu, const jbc::ir::Insn& in)
{
    if (in.op != jbc::core::Op::GOTO && in.op != jbc::core::Op::GOTO_W) return false;
    auto resolved = cu.dom.resolveLabel(in.target);
    if (resolved.is_err()) return false;
    const jbc::ir::LabelTarget t = resolved.unwrap();
    if (t.isEnd()) return false;
    const jbc::ir::Insn* tgt = cu.dom.peek(t.insn);
    return tgt != nullptr
        && tgt->srcPc == in.srcPc + (in.op == jbc::core::Op::GOTO_W ? 5u : 3u);
}

/// @brief 按求值结果造一条常量装载：短形态优先，放不下才落池。
///
/// 池索引走 `CodeEditor::loadConst` 选形态，`LDC` 的操作数域只有 u1，大类
/// 的池索引轻易超过 255，编码器对超界 `LDC` 直接 Unencodable（insn_codec
/// 的域宽校验）。`loadConst` 负责按实际索引在 `LDC`/`LDC_W` 之间选对。
inline bool makeConstLoad(jbc::ir::ClassSession& session, const jsim::SimValue& v,
                          jbc::ir::Insn& out)
{
    if (!v.known) return false;
    switch (v.type) {
    case jsim::ValType::I32: {
        const auto n = static_cast<ca::i32>(v.intBits);
        auto r = mjbase::asm_insn::intConstInsn(n);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.internInteger(n);
        if (cp == 0) return false;
        out = jbc::ir::CodeEditor::loadConst(cp);
        return true;
    }
    case jsim::ValType::I64: {
        auto r = mjbase::asm_insn::longConstInsn(v.intBits);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addLong(v.intBits);
        if (cp == 0 || cp > 0xFFFF) return false;   // LDC2_W 是 CP2 域（u2）
        out.op = jbc::core::Op::LDC2_W;
        out.cp = cp;
        return true;
    }
    case jsim::ValType::F32: {
        const float f = v.asF32();
        auto r = mjbase::asm_insn::floatConstInsn(f);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addFloatBits(jsim::floatToBits(f));
        if (cp == 0) return false;
        out = jbc::ir::CodeEditor::loadConst(cp);
        return true;
    }
    case jsim::ValType::F64: {
        const double d = v.asF64();
        auto r = mjbase::asm_insn::doubleConstInsn(d);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addDoubleBits(jsim::doubleToBits(d));
        if (cp == 0 || cp > 0xFFFF) return false;
        out.op = jbc::core::Op::LDC2_W;
        out.cp = cp;
        return true;
    }
    default: return false;   // Ref/Null 不落常量装载
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 帧链写回（擦除系残留共用）
//
// 为什么必须是 mjbase 帧链而不是手剪 SMT：折叠/擦除后，锚在被擦指令上的
// 帧、锚在 NOP 化锚点上的帧，内容都描述改写前的栈形，逐帧修补修不完
// （v5 round1 实测 verifyFail=1412，基线 146）。mjbase 的正统写法是
// prepareNestedAttrsForLayout（LNT/LVT 悬垂吸附、SMT 悬垂整体摘除）→
// lowerMethod → rewireFramesAfterLower（Strict 数据流从代码真值重算全部
// 帧 + maxes 再生挂回）。帧内容永远是重算的，不存在陈旧。
//
// promote 纪律（v5 教训）：promoteMethod 每次调用都从 backing 的 Code
// 字节重建 mu->code，同方法只准 promote 一次，DOM 改写、prepare、lower
// 全落在同一 unit 上；中间任何再 promote 都会静默弃掉全部改写。

/// @brief CLI `--jdk` 装填的 JDK 类最小骨架种子（FrameContext 与 CLI 每轮
/// 层次图装配共用；空 = 无 JDK 旁证的历史口径）。
///
/// 只装 this/super/接口/access（成员表清空：resolver 的 lub 只走父链/接口
/// 闭包，成员面无消费方）；键驻留装填侧专属池，消费方换池重驻留。装填入口
/// 在 pipeline_adapter（ensureJdkHierarchySeed），CLI 于管线启动前调用。
inline std::vector<mjbase::hierarchy::ClassSummary> gJdkHierarchySeed;
/// @brief 种子按 dst 池换驻后的整体副本（FrameContext / CLI 每轮层次图装配
/// 共用一条换池路径；种子空时返回空表）。实现与装填同源在 pipeline_adapter。
std::vector<mjbase::hierarchy::ClassSummary> jdkSeededSummaries(mjbase::KeyPool& dst);

/// @brief 种子摘要换池重驻留（self/super/接口名逐一 intern 进目标池）。
inline mjbase::hierarchy::ClassSummary reseedSummary(
    mjbase::KeyPool& dst, const mjbase::KeyPool& src,
    const mjbase::hierarchy::ClassSummary& s)
{
    mjbase::hierarchy::ClassSummary out;
    const auto keyOf = [&](mjbase::ClassKey k) {
        return k.valid() ? mjbase::classKeyOf(dst, src.str(k.id)) : mjbase::ClassKey{};
    };
    out.self  = keyOf(s.self);
    out.super = keyOf(s.super);
    out.interfaces.reserve(s.interfaces.size());
    for (const mjbase::ClassKey k : s.interfaces) out.interfaces.push_back(keyOf(k));
    out.access = s.access;
    return out;
}

/// @brief 容器级帧重算上下文：Strict 求解所需的类层级 + resolver 适配器。
///
/// summary 与 session 解绑：构造期逐类开会话 → fillSummary 抽骨架 → addClass 入库 → 会话当场弃置，任何时刻至多一个会话在场。解绑的依据是 fillSummary 的产出只有两处自持落点，pool_（内容寻址驻留池，arena 只增不移，视图随池存活）与 hierarchy_ 入库的 ClassSummary 值拷贝（键全是 pool_ 的 u32 id，vector 拷贝即快照）；byName_ 的名字视图同样借用 pool_ 而非会话符号池。构造完成后没有任何数据活引用 session，rewrite 阶段按类重开会话（容器里有原始字节），Strict 帧重算的 CommonSuperClassResolver 只读层级骨架，照常工作。
///
/// 遍历序沿用原 scanClassResources 的容器资源序（visitResources），驻留键的 intern 序逐位一致 → 键 id、环/重复登记的判定时点、resolver 查询缓存全部不动，这是 trace/产物逐位等价的前提。不挂 pass 级时间片检查：层次图必须盖全容器（与原口径一致，空条目与畸形类不产出事实，fail-open）。
///
/// 容器层骨架之后追加 `gJdkHierarchySeed`（CLI `--jdk` 装填的 JDK 类最小骨
/// 架）：层级图只盖容器时，Strict 求解在「两个容器外类」的合并点上求不出
/// 真 lub（InputStream×GZIPInputStream 落成 Object/top），帧不可校验——
/// jsoup v3_string DataUtil.openStream 实证（string 折叠改保真重定位后由
/// indy_restore 折叠同一方法复现）。种子类是 jmods 直读的最小骨架（成员表
/// 清空：resolver 的 lub 只走父链/接口闭包，成员面无消费方）。
///
/// 成员间是借用关系（adapter 借 super，super 借 hierarchy，hierarchy 借 pool），禁拷贝禁移动；按 rewrite() 单次调用于栈上持有。
class FrameContext
{
public:
    explicit FrameContext(const mjbase::model::ResourceContainer& container)
    {
        container.visitResources(
            [this](std::string_view, const mjbase::model::IResource& resource) {
                if (resource.getType() != mjbase::model::ResourceType::Class) return;
                const auto* cls =
                    ca::core::tag_cast::cast<const mjbase::model::ClassResource>(&resource);
                if (cls == nullptr || cls->get().empty()) return;
                auto session = jbc::ir::makeSessionFromBytes(
                    jbc::core::ByteView{cls->get().data(), cls->get().size()});
                if (session.is_err()) return;
                (void)hierarchy_.addClass(
                    mjbase::hierarchy::fillSummary(*session.unwrap(), pool_));
                // session 随作用域结束弃置：骨架已收进 pool_ 与 hierarchy_。
            });
        // JDK 种子（键驻 fill 侧种子池；addClass 拷贝入库时按文本在本 pool_
        // 重新 intern，借用关系不跨池）。空种子 = 历史口径。
        for (const auto& summary : jdkSeededSummaries(pool_)) {
            (void)hierarchy_.addClass(summary);
        }
    }

    jbc::analysis::ITypeResolver& resolver() { return adapter_; }

private:
    mjbase::KeyPool                        pool_;
    mjbase::hierarchy::ClassHierarchy      hierarchy_{pool_};
    mjbase::spi::CommonSuperClassResolver  super_{hierarchy_};
    mjbase::spi::StrictTypeResolverAdapter adapter_{super_};
};

/// @brief 单方法改写驱动：promote 一次 → mutate → 帧链正统写回。@return 是否改写成功。
///
/// mutate(CodeUnit&) 返回是否改动了 DOM（false = 无改动，不写回）。
/// 改动时走 prepare → lower → rewire。jsr/ret 等 Strict 不可算形态以异常
/// 抛出，这里收敛为 false，调用方 fail-closed 整类放弃，不落半截。
template<typename MutateFn>
bool mutateMethodWithFrames(jbc::ir::ClassSession& s, ca::usize methodIndex,
                            jbc::analysis::ITypeResolver& resolver, MutateFn&& mutate)
{
    if (!jbc::ir::hasCode(s, methodIndex)) return false;
    auto promoted = jbc::ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return false;
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;

    bool changed = false;
    try {
        changed = mutate(*cu);
    } catch (const std::exception&) {
        return false;
    }
    if (!changed) return false;

    try {
        // resolver 必注入：SMT 摘除后由 rewire 再生，不会落无帧类。
        (void)mjbase::spi::frame_rewire::prepareNestedAttrsForLayout(s, *cu);
        auto lowered = jbc::ir::lowerMethod(s, methodIndex);
        if (lowered.is_err()) {
            // GRUNT_FRAME_TRACE=1：打印 lower 失败原因（帧链失败定位面）。
            if (std::getenv("GRUNT_FRAME_TRACE") != nullptr) {
                std::fprintf(stderr, "[grunt.Frame] lower err: %s\n",
                             jbc::ir::to_string(lowered.unwrap_err()));
            }
            return false;
        }

        const jbc::ir::ClassUnit& unit   = s.unit();
        const jbc::ir::MethodUnit& mu    = unit.methods[methodIndex];
        const std::string_view name      = unit.symbols().str(mu.name);
        const std::string_view desc      = unit.symbols().str(mu.desc);

        mjbase::KeyPool        pool;   // 身份池只服务 MethodScope 构造
        const mjbase::ClassKey ownerKey =
            mjbase::classKeyOf(pool, unit.symbols().str(unit.thisClass));
        const mjbase::MemberKey key = mjbase::memberKeyOf(pool, ownerKey, name, desc);
        mjbase::spi::MethodScope scope{
            mjbase::spi::MethodTarget{key, mu.id, name, desc, mu.access, s}, *cu};
        mjbase::spi::frame_rewire::rewireFramesAfterLower(s, scope, resolver);
        return true;
    } catch (const std::exception& e) {
        // GRUNT_FRAME_TRACE=1：打印 Strict 帧重算异常（帧链失败定位面；
        // 恒 false 语义不变，诊断只在 env 检查下多打一行 stderr）。
        if (std::getenv("GRUNT_FRAME_TRACE") != nullptr) {
            std::fprintf(stderr, "[grunt.Frame] rewire exc: %s\n", e.what());
        }
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 资源删除（资源面改写动词：本仓首个非类条目写原语）
//
// 现役画布（container.hpp 的 forEachClass / replaceClass）只走类资源；资源
// 删除是容器条目的结构性变化，单独成原语收口在这里（写侧），核对在
// proof_toolbox（读侧）。删除只摘条目、不碰类面；删哪个由各残留的
// scan 产出（Era=Kasumi 检查在 scan），本原语不做任何家族判断。
// ═══════════════════════════════════════════════════════════════════════════

/// @brief 按 uri 从容器摘除一个资源条目（类与非类条目同一口径）。
/// @return 是否确有此条目被摘除（缺失 = false，调用方按核对违例计数）。
///
/// 键视图驻留容器 arena（unordered_map<std::string_view> 按内容相等查找），
/// 等价字符串视图可直查； erase 只摘 map 条目，URI arena 文本不回收
/// （arena 只增不移，条目名后续核对引用无悬垂风险）。
inline bool removeResource(mjbase::model::ResourceContainer& container, std::string_view uri)
{
    auto& resources = container.getResources();
    const auto it = resources.find(uri);
    if (it == resources.end()) return false;
    resources.erase(it);
    return true;
}


// ═══════════════════════════════════════════════════════════════════════════
// 明文码元 → 常量池（string_pool_fold 的改写原语单源化，pool_array_fold 同用）
// ═══════════════════════════════════════════════════════════════════════════

/// @brief UTF-16 码元序列 → classfile modified UTF-8（NUL 走 0xC0 0x80；
///        代理对按码元各落 3 字节形，javac 写常量池的同构口径）。全码元
///        可表示，无失败面。
inline void u16ToMutf8(const std::vector<ca::u16>& units, std::string& out)
{
    out.clear();
    for (const ca::u16 u : units) {
        if (u == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
}

/// @brief UTF-16 码元序列 → UTF-8 记录形（有效代理对重组 4 字节；孤立代理
///        按码元落 3 字节形，与 u16ToMutf8 池写口径同构）。记录形进
///        Site.plaintext（trace/统计/单测）+ 各折叠的明文门。全码元可表
///        示，恒返 true。
///        孤立代理语料证据（q8 bD.t）：Grunt 加密的是裸 UTF-16 码元序列，
///        明文可含孤立代理（U+DEFA^0x1C3 = 0xDF39）；classfile
///        CONSTANT_Utf8 修饰 UTF-8 按码元 3 字节形同可表示。旧口径（孤立
///        代理整站拒）预设"明文必过 javac 源语言面"——池写面 u16ToMutf8
///        早就不设此限，记录门不比池写更严。
inline bool u16ToUtf8Record(const std::vector<ca::u16>& units, std::string& out)
{
    out.clear();
    for (ca::usize i = 0; i < units.size(); ++i) {
        const ca::u16 u = units[i];
        const bool pairable = u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size()
                              && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF;
        if (pairable) {
            const ca::u32 cp = 0x10000u
                             + (((static_cast<ca::u32>(u) - 0xD800u) << 10)
                                | (static_cast<ca::u32>(units[i + 1]) - 0xDC00u));
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            ++i;
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return true;
}

/// @brief 明文 intern 进常量池：码元 → MUTF-8 → Utf8（命中既有池不膨胀）+
///        CONSTANT_String 包装。@return String 条目索引；0 = 失败（占位槽
///        索引天然是哨兵）。空明文合法：解密结果可以是空串（u.class 实证，
///        池填充点存 ""）——空 Utf8 常量照常落池，不走 0 哨兵。
///
/// findUtf8 占位槽陷阱（u.class write-fail Unencodable 实证根因）：ldc2_w
/// 落 Long 条目后跟一个 placeholder 槽，其 Entry 默认 tag=Utf8、utf8Len=0；
/// libjbc findUtf8("") 不跳 placeholder，会把占位槽当既有空 Utf8 命中，
/// addString 引到占位槽上 → validateReferences bad_constant_pool_ref →
/// writeClass Unencodable，整类写盘被弃。非空明文不触发（长度不匹配）。
/// 命中结果验 valid()，占位槽一律视作 miss 补 intern 真条目。
inline ca::u32 internPlainString(jbc::core::ConstantPool& cp, const std::vector<ca::u16>& plain)
{
    std::string mutf8;
    u16ToMutf8(plain, mutf8);
    ca::u32 utf8Idx = cp.findUtf8(mutf8);
    if (utf8Idx == 0 || !cp.valid(utf8Idx)) {
        utf8Idx = cp.addUtf8(mutf8);
        if (utf8Idx == 0 || !cp.valid(utf8Idx)) return 0;
    }
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        const jbc::core::ConstantPool::Entry& e = cp.at(i);
        if (e.tag == jbc::core::CpTag::String && e.a == utf8Idx) return i;
    }
    return cp.addString(utf8Idx);
}

/// @brief 方法体的 **pc 布局序**只读视图（srcPc 升序；kNoPc 指令不在
///        backing 布局上，不进视图）。
///
/// 布局窗扫描（key 窗 / 回收集 / 蹦床窗这类「紧邻 N 条」判定）一律走本
/// 视图，不走 dom.order()——order 不保证 pc 升序（循环回边 / 异常 handler
/// 等重排），位置相邻判据在带环方法上静默错窗（indy_restore 260705
/// keyMissing=984 同族实证，2026-11-17 修）。
inline std::vector<const jbc::ir::Insn*> pcOrderedInsns(const jbc::ir::CodeUnit& cu)
{
    std::vector<const jbc::ir::Insn*> out;
    const auto&                       order = cu.dom.order();
    out.reserve(order.size());
    for (const jbc::ir::InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in != nullptr && in->srcPc != jbc::ir::kNoPc) out.push_back(in);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const jbc::ir::Insn* a, const jbc::ir::Insn* b) {
                         return a->srcPc < b->srcPc;
                     });
    return out;
}

/// @brief pc 是否在列表中（小列表线性扫，fold 类改写受害面通常个位数）。
inline bool pcInList(const std::vector<ca::u32>& list, ca::u32 pc)
{
    return std::find(list.begin(), list.end(), pc) != list.end();
}

/// @brief 剪掉锚在受害者 pc 上的 LNT/LVT 行（擦除口径的属性配套面）。
///
/// 擦除让受害者旧 pc 从 pcmap 蒸发，行锚 miss 即 Err(RelocationBadTarget)
/// （number_fold B#5/c#3 实证：命名形状站点擦除后整方法 lower 失败连坐）。
/// 行是调试面，删行无损语义；LVT 行的终点落在受害者上同样 miss，一并剪。
/// 前置：方法已 promote 与否皆可（本入口只走 backing 属性）。@return false =
/// 属性判读不了，调用方整站放弃；true = 剪完（或无行面可剪）。
inline bool prunePcAttrs(jbc::core::ClassFile& cf, jbc::core::MemberInfo& member,
                         const std::vector<ca::u32>& victimPcs)
{
    auto codeR = cf.decodeCode(member);
    if (codeR.is_err()) return false;
    jbc::core::CodeAttr* code = codeR.unwrap();
    if (code == nullptr) return true;   ///< 无 Code = 无行面
    for (jbc::core::AttrSlot& slot : code->attrs) {
        const std::string_view n = cf.utf8(slot.nameCp);
        const bool isLnt = n == "LineNumberTable";
        const bool isLvt = n == "LocalVariableTable" || n == "LocalVariableTypeTable";
        if (!isLnt && !isLvt) continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        if (d.unwrap() == nullptr) return false;
        const auto kind = d.unwrap()->kind();
        if (kind != jbc::core::AttrKind::LineNumberTable
            && kind != jbc::core::AttrKind::LocalVariableTable) {
            return false;
        }
        auto* body = slot.mutate();   ///< 置 Modified：写回走重编码
        if (body == nullptr) return false;
        if (isLnt) {
            auto& lnt = static_cast<jbc::core::LineNumberTableAttr&>(*body);
            std::vector<jbc::core::LineNumberTableAttr::Row> kept;
            kept.reserve(lnt.rows.size());
            for (auto& r : lnt.rows) {
                if (pcInList(victimPcs, r.startPc)) continue;
                kept.push_back(r);
            }
            lnt.rows = std::move(kept);
        } else {
            auto& lvt = static_cast<jbc::core::LocalVariableTableAttr&>(*body);
            std::vector<jbc::core::LocalVariableTableAttr::Row> kept;
            kept.reserve(lvt.rows.size());
            for (auto& r : lvt.rows) {
                if (pcInList(victimPcs, r.startPc)
                    || pcInList(victimPcs, r.startPc + r.length)) {
                    continue;
                }
                kept.push_back(r);
            }
            lvt.rows = std::move(kept);
        }
    }
    return true;
}

}   // namespace grunt::rewrite_tools
