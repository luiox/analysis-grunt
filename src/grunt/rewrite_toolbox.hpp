/// @file        rewrite_toolbox.hpp
/// @brief       REWRITE 工具箱：跨残留共享的改写原语。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口两组原语：
/// 「按求值结果造一条常量装载」（constant_call / const_method 改写段共用）
/// 与「单 promote + mjbase 帧链写回」（predicate_fold / dead_code /
/// const_value_promote / seed_strip 的改写驱动共用）。
#pragma once

#include <cstring>
#include <exception>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/frame.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/asm/evaluator/value.hpp"
#include "mjbase/asm/insn/insn_factory.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace grunt::rewrite_tools {

template <class To, class From>
To bitCast(const From& from) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "bitCast size mismatch");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

/// @brief 按求值结果造一条常量装载：短形态优先，放不下才落池。
///
/// 池索引走 `CodeEditor::loadConst` 选形态——`LDC` 的操作数域只有 u1，大类
/// 的池索引轻易超过 255，编码器对超界 `LDC` 直接 Unencodable（insn_codec
/// 的域宽校验）。`loadConst` 负责按实际索引在 `LDC`/`LDC_W` 之间选对。
inline bool makeConstLoad(jbc::ir::ClassSession& session, const mjbase::asm_evaluator::Value& v,
                          jbc::ir::Insn& out)
{
    switch (v.kind) {
    case mjbase::asm_evaluator::Value::Kind::I32: {
        auto r = mjbase::asm_insn::intConstInsn(v.i32);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.internInteger(v.i32);
        if (cp == 0) return false;
        out = jbc::ir::CodeEditor::loadConst(cp);
        return true;
    }
    case mjbase::asm_evaluator::Value::Kind::I64: {
        auto r = mjbase::asm_insn::longConstInsn(v.i64);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addLong(v.i64);
        if (cp == 0 || cp > 0xFFFF) return false;   // LDC2_W 是 CP2 域（u2）
        out.op = jbc::core::Op::LDC2_W;
        out.cp = cp;
        return true;
    }
    case mjbase::asm_evaluator::Value::Kind::F32: {
        auto r = mjbase::asm_insn::floatConstInsn(v.f32);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addFloatBits(bitCast<ca::u32>(v.f32));
        if (cp == 0) return false;
        out = jbc::ir::CodeEditor::loadConst(cp);
        return true;
    }
    case mjbase::asm_evaluator::Value::Kind::F64: {
        auto r = mjbase::asm_insn::doubleConstInsn(v.f64);
        if (r.is_ok()) {
            out = r.unwrap();
            return true;
        }
        const ca::u32 cp = session.backing().cp.addDoubleBits(bitCast<ca::u64>(v.f64));
        if (cp == 0 || cp > 0xFFFF) return false;
        out.op = jbc::core::Op::LDC2_W;
        out.cp = cp;
        return true;
    }
    default: return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 帧链写回（擦除系残留共用）
//
// 为什么必须是 mjbase 帧链而不是手剪 SMT：折叠/擦除后，锚在被擦指令上的
// 帧、锚在 NOP 化锚点上的帧，内容都描述改写前的栈形——逐帧修补修不完
// （v5 round1 实测 verifyFail=1412，基线 146）。mjbase 的正统写法是
// prepareNestedAttrsForLayout（LNT/LVT 悬垂吸附、SMT 悬垂整体摘除）→
// lowerMethod → rewireFramesAfterLower（Strict 数据流从代码真值重算全部
// 帧 + maxes 再生挂回）。帧内容永远是重算的，不存在陈旧。
//
// promote 纪律（v5 教训）：promoteMethod 每次调用都从 backing 的 Code
// 字节重建 mu->code——同方法只准 promote 一次，DOM 改写、prepare、lower
// 全落在同一 unit 上；中间任何再 promote 都会静默弃掉全部改写。

/// @brief 容器级帧重算上下文：Strict 求解所需的类层级 + resolver 适配器。
///
/// 成员间是借用关系（adapter 借 super，super 借 hierarchy，hierarchy 借
/// pool），禁拷贝禁移动；按 rewrite() 单次调用于栈上持有。scanned_ 持有
/// 会话保活（fillSummary 只读骨架，session 死则 summary 的符号视图悬垂）。
class FrameContext
{
public:
    explicit FrameContext(const mjbase::model::ResourceContainer& container)
        : scanned_(mjbase::analysis::scanClassResources(container))
    {
        for (const mjbase::analysis::ScannedClass& sc : scanned_) {
            if (!sc.session) continue;
            (void)hierarchy_.addClass(mjbase::hierarchy::fillSummary(*sc.session, pool_));
        }
    }

    jbc::analysis::ITypeResolver& resolver() { return adapter_; }

private:
    mjbase::KeyPool                             pool_;
    std::vector<mjbase::analysis::ScannedClass> scanned_;
    mjbase::hierarchy::ClassHierarchy           hierarchy_{pool_};
    mjbase::spi::CommonSuperClassResolver       super_{hierarchy_};
    mjbase::spi::StrictTypeResolverAdapter      adapter_{super_};
};

/// @brief 单方法改写驱动：promote 一次 → mutate → 帧链正统写回。@return 是否改写成功。
///
/// mutate(CodeUnit&) 返回是否改动了 DOM（false = 无改动，不写回）。
/// 改动时走 prepare → lower → rewire。jsr/ret 等 Strict 不可算形态以异常
/// 抛出，这里收敛为 false——调用方 fail-closed 整类放弃，不落半截。
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
        if (lowered.is_err()) return false;

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
    } catch (const std::exception&) {
        return false;
    }
}

}   // namespace grunt::rewrite_tools
