/// @file        field_census.hpp
/// @brief       家族知识：容器级字段访问面普查（promote 精确口径）。
///
/// 消费方：const_value_promote（唯一写入门）、seed_strip（写全在本类 clinit
/// 门）、dead_member（零引用门）。普查走 promote 后的指令面，不受死池条目
/// 干扰；key = owner::name。
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
    bool        inClinit    = false;   ///< 访问者方法是 <clinit>
};

/// @brief 逐字段访问清单，key = `owner::name`。
using Census = std::unordered_map<std::string, std::vector<FieldAcc>>;

/// @brief 容器级普查：全部方法的字段访问指令逐条归类。
inline void scanFieldAccess(const mjbase::model::ResourceContainer& container, Census& out)
{
    namespace ir = jbc::ir;
    using jbc::core::Op;

    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            const bool isClinit = unit.symbols().str(unit.methods[mi].name) == "<clinit>";
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
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
}

}   // namespace grunt::field_census
