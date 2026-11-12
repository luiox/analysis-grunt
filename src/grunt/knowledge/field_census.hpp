/// @file        field_census.hpp
/// @brief       家族知识：容器级字段访问面普查（promote 精确口径）。
///
/// 消费方：const_value_promote（唯一写入检查）、seed_strip（写全在本类 clinit
/// 检查）。普查走 promote 后的指令面，不受死池条目干扰；key = owner::name。
///
/// fail-closed 口径（与 dead_member 引用普查同一课）：普查**不完整**时调用方
/// 必须放弃改写，会话打不开的类、promote 失败的方法都计进 Stat，键面看不见
/// 它们的访问，任何零引用/唯一写判据都会漏计证据（宁误留不误删）。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"

namespace grunt::field_census {

/// @brief 一次字段访问。
struct FieldAcc
{
    std::string uri;        ///< 访问者所在类
    ca::usize   methodIndex = 0;
    bool        write       = false;   ///< PUTSTATIC / PUTFIELD
    bool        inClinit    = false;   ///< 访问者方法是 `<clinit>`
};

/// @brief 逐字段访问清单，key = `owner::name`。
using Census = std::unordered_map<std::string, std::vector<FieldAcc>>;

/// @brief 普查完整性统计：漏计的类 / 方法数（>0 即引用证据不足）。
struct Stat
{
    ca::usize classesSkipped       = 0;   ///< 会话打不开，整类未普查
    ca::usize methodsPromoteFailed = 0;   ///< promote 失败，方法访问面缺失

    bool complete() const noexcept
    {
        return classesSkipped == 0 && methodsPromoteFailed == 0;
    }
};

/// @brief 容器级普查：全部方法的字段访问指令逐条归类。@return 完整性统计。
inline Stat scanFieldAccess(const mjbase::model::ResourceContainer& container, Census& out)
{
    namespace ir = jbc::ir;
    using jbc::core::Op;

    Stat stat;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++stat.classesSkipped;
            return;
        }
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            const bool isClinit = unit.symbols().str(unit.methods[mi].name) == "<clinit>";
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) {
                ++stat.methodsPromoteFailed;
                continue;
            }
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) {
                ++stat.methodsPromoteFailed;
                continue;
            }
            for (const ir::InsnId id : cu->dom.order()) {
                const ir::Insn* in = cu->dom.peek(id);
                if (in == nullptr) continue;
                if (in->op != Op::GETSTATIC && in->op != Op::PUTSTATIC
                    && in->op != Op::GETFIELD && in->op != Op::PUTFIELD) {
                    continue;
                }
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*in, sem)) continue;
                std::string key;
                key.reserve(sem.owner.size() + sem.name.size() + 2);
                key += sem.owner;
                key += "::";
                key += sem.name;
                out[key].push_back(FieldAcc{std::string(entry.uri), mi,
                                            in->op == Op::PUTSTATIC
                                                || in->op == Op::PUTFIELD,
                                            isClinit});
            }
        }
    });
    return stat;
}

}   // namespace grunt::field_census
