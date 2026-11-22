/// @file        scan_toolbox.hpp
/// @brief       SCAN 工具箱：跨残留共享的扫描判定、码元面编解码与方法索引。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口五件：
/// `classInitNeutral`，owner 类 `<clinit>` 的中性判定，被
/// constant_call（求值前的类初始化检查）与 proxy_inline（owner 中性检查）共用；
/// `bsmEntries`，类级 BootstrapMethods 条目表读取，被 indy_fold /
/// indy_decode 共用；`mutf8ToUnits`/`decryptXor`，MUTF-8 码元面解码与逐码
/// 元自逆 XOR，被 indy_restore / indy_decode / material_key_restore 共用
/// （原三 pass 各持同形私有副本，B2 单源化）；`MethodIndex`/
/// `buildMethodIndex`，容器方法索引（keyOf 复合键），constant_call 与
/// gate_processor_fold 同源共用（原两 pass 各持副本，B2 单源化），
/// `keyOf` 另被 proxy_inline / field_access_proxy / const_method 的定义表
/// 共用（原五 pass 各持同形副本）。
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"

#include "grunt/container.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "jsim/engine.hpp"
#include "jsim/value.hpp"

namespace grunt::scan_tools {

/// @brief MUTF-8 → UTF-16 码元（不经严格 UTF-8 解码：密文里合法地存在
///        Java 孤代理码元，classfile 以 CESU-8 形 3 字节承载，严格 UTF-8
///        解码器会拒收，而加密语义只在码元层面异或）。MUTF-8 的 NUL 特例
///        （0xC0 0x80）一并处理；4 字节形 / 挂续字节（classfile Utf8 不出现）
///        返回 false。
inline bool mutf8ToUnits(std::string_view in, std::vector<ca::u16>& out)
{
    out.clear();
    ca::usize i = 0;
    while (i < in.size()) {
        const ca::u8 b0 = static_cast<ca::u8>(in[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<ca::u16>(b0));
            ++i;
            continue;
        }
        if ((b0 & 0xE0) == 0xC0) {
            if (i + 1 >= in.size()) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            if ((b1 & 0xC0) != 0x80) return false;
            out.push_back(static_cast<ca::u16>(((b0 & 0x1F) << 6) | (b1 & 0x3F)));
            i += 2;
            continue;
        }
        if ((b0 & 0xF0) == 0xE0) {
            if (i + 2 >= in.size()) return false;
            const ca::u8 b1 = static_cast<ca::u8>(in[i + 1]);
            const ca::u8 b2 = static_cast<ca::u8>(in[i + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            out.push_back(static_cast<ca::u16>(((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6)
                                               | (b2 & 0x3F)));
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

/// @brief 逐码元自逆 XOR（每 UTF-16 码元 XOR key 低 16 位，Java `char ^ int`
///        后 `i2c` 的语义），产物落 UTF-8，异常码元按 3 字节原样承载，垃圾
///        解密结果会死在下游 desc 核对，不在这里猜。@return false = MUTF-8
///        解码失败。
inline bool decryptXor(std::string_view encrypted, ca::i32 key, std::string& out)
{
    std::vector<ca::u16> units;
    if (!mutf8ToUnits(encrypted, units)) return false;
    const ca::u16 lo = static_cast<ca::u16>(key);
    out.clear();
    for (ca::u16& u : units) {
        u = static_cast<ca::u16>(u ^ lo);
        if (u < 0x80) {
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

/// @brief 取类级 BootstrapMethods 的条目表（没有属性就是空表）。
///
/// 解码前先过注解族深度预检（mjbase #158）：BootstrapMethods 本身不在注解族，
/// 守卫对它恒放行；挂同一道检查是让一切 decodeAttr 直调位保持单一形状，将来
/// 若有注解族解码落进来，守卫已在位。
inline const std::vector<jbc::core::BootstrapMethodsAttr::Entry>* bsmEntries(
    jbc::ir::ClassSession& s)
{
    jbc::core::ClassFile& cf = s.backing();
    jbc::core::AttrSlot*  slot = cf.findClassAttr("BootstrapMethods");
    if (slot == nullptr) return nullptr;
    if (!mjbase::util::attrSafeToDecode(cf, *slot)) return nullptr;
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) return nullptr;
    // 不用 dynamic_cast：本仓关 RTTI（/GR-），走 libca 的 tag_cast 约定
    // （AttrBody::kind() 是它的类型标签）。
    auto* attr = ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(decoded.unwrap());
    return attr == nullptr ? nullptr : &attr->entries;
}

/// @brief 判 owner 类 `<clinit>` 是否中性：删掉指向 owner 的一次主动使用
///        （invokestatic 调用、字段访问内联）不改变可观察行为。
///
/// 两个接受面（都是保守面而不是判据）：
///
/// 1. **缺失或空壳**：无 `<clinit>`、无码，或只剩 NOP/RETURN 且无异常表；
/// 2. **纯常量初值**：clinit 只由常量装载（数值/String）、栈整形、数值
///    转换与对**本类字段**的 GETSTATIC/PUTSTATIC 构成（watermark 面：
///    Grunt 在每个类撒 `ldc "PROTECTED BY …"; putstatic 自家字段`）。这一
///    面的副作用 = 对本类静态字段写确定值——初始化提前/推迟/不发生都不可
///    观察：字段值不依赖时机；字段若有读者，读者本身就是主动使用，初始化
///    照常发生且写的是同一个确定值。
///
/// 出现分支、方法调用、NEW、athrow、异常表、任何对**本类以外**字段的访问
/// 即判非中性（fail-closed）。消费方四个（ConstantCall / GateProcessorFold /
/// ProxyInline / FieldAccessProxy），语义同一：折叠掉指向 owner 的一次主动
/// 使用。
inline bool classInitNeutral(jbc::ir::ClassSession& session)
{
    namespace ir = jbc::ir;
    using jbc::core::Op;

    constexpr ca::usize kNoIndex = static_cast<ca::usize>(-1);

    const ir::ClassUnit& unit        = session.unit();
    ca::usize            clinitIndex = kNoIndex;
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) == "<clinit>") {
            clinitIndex = i;
            break;
        }
    }
    if (clinitIndex == kNoIndex) return true;
    if (!ir::hasCode(session, clinitIndex)) return true;

    auto promoted = ir::promoteMethod(session, clinitIndex);
    if (promoted.is_err()) return false;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;
    if (!cu->exc.empty()) return false;

    bool empty = true;
    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (in->op == Op::NOP || in->op == Op::RETURN) continue;
        empty = false;
        break;
    }
    if (empty) return true;

    // 纯常量初值面：watermark 形（常量装载 + putstatic 本类字段 + return）。
    const std::string_view self = session.backing().thisName();
    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        switch (in->op) {
        case Op::NOP:
        case Op::RETURN:
        case Op::ACONST_NULL:
        case Op::ICONST_M1:
        case Op::ICONST_0:
        case Op::ICONST_1:
        case Op::ICONST_2:
        case Op::ICONST_3:
        case Op::ICONST_4:
        case Op::ICONST_5:
        case Op::BIPUSH:
        case Op::SIPUSH:
        case Op::LCONST_0:
        case Op::LCONST_1:
        case Op::FCONST_0:
        case Op::FCONST_1:
        case Op::FCONST_2:
        case Op::DCONST_0:
        case Op::DCONST_1:
        case Op::I2L:
        case Op::I2F:
        case Op::I2D:
        case Op::L2I:
        case Op::L2F:
        case Op::L2D:
        case Op::F2I:
        case Op::F2L:
        case Op::F2D:
        case Op::D2I:
        case Op::D2L:
        case Op::D2F:
        case Op::I2B:
        case Op::I2C:
        case Op::I2S:
        case Op::POP:
        case Op::POP2:
        case Op::DUP:
        case Op::DUP2:
        case Op::SWAP:
            continue;
        case Op::LDC:
        case Op::LDC_W:
        case Op::LDC2_W: {
            ir::ClassSession::ConstSem sem;
            if (!session.constOf(*in, sem)) return false;
            using CK = ir::ClassSession::ConstSem::Kind;
            if (in->op == Op::LDC2_W) {
                if (sem.kind != CK::Long && sem.kind != CK::Double) return false;
            } else if (sem.kind != CK::Int && sem.kind != CK::String) {
                return false;
            }
            continue;
        }
        case Op::GETSTATIC:
        case Op::PUTSTATIC: {
            ir::ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)
                || sem.kind != ir::ClassSession::MemberSem::Kind::Field
                || sem.owner != self) {
                return false;
            }
            continue;
        }
        default:
            return false;
        }
    }
    return true;
}

/// @brief `owner\nname\ndesc` 复合键（方法索引与各定义表共用）。
///
/// 原 constant_call / gate_processor_fold / proxy_inline / field_access_proxy
/// / const_method 五份同形私有副本单源化至此（B2）。
inline std::string keyOf(std::string_view owner, std::string_view name,
                         std::string_view desc)
{
    std::string k;
    k.reserve(owner.size() + name.size() + desc.size() + 2);
    k.append(owner);
    k.push_back('\n');
    k.append(name);
    k.push_back('\n');
    k.append(desc);
    return k;
}

/// @brief 容器方法定位值（constant_call / gate_processor_fold 同源）。
///
/// 原两份副本只在 constant_call 多一个无人读的 `desc` 字段，统一形落在
/// 三字段（B2 单源化，逐处登记）。
struct MethodLoc
{
    std::string owner;
    ca::usize   index  = 0;
    ca::u32     access = 0;
};

/// @brief 容器方法索引：`keyOf` 键 → 声明位置。
using MethodIndex = std::unordered_map<std::string, MethodLoc>;

/// @brief 重名类（两个容器条目 `thisName` 撞同一 `keyOf` 键）的取舍口径。
///
/// 两 pass 原生有别且不可猜统一：constant_call 是 `emplace`（先到先得），
/// gate_processor_fold 是 `insert_or_assign`（后到后得），B2 单源化时以
/// 本枚举逐处钉住原口径，行为逐位不动。
enum class IndexDup : ca::u8 { KeepFirst, KeepLast };

/// @brief 容器方法索引构建（constant_call / gate_processor_fold 的
///        `buildIndex` 同源收口）：每类开只读会话，全方法入索引。
///
/// guard 是调用方带入的 per-class fail-closed 包裹（本仓 scanGuard 面，
/// stderr 前缀随 pass 各异，不在此固化）：guard 返回 false = 该类跳过、
/// 余类继续。遍历走 container.hpp 的 `forEachClass`，pass 级时间片检查语义
/// 与原两份副本逐位同。
template <class Guard>
inline void buildMethodIndex(const mjbase::model::ResourceContainer& container,
                             MethodIndex& index, IndexDup dup, Guard&& guard)
{
    namespace ir = jbc::ir;

    forEachClass(container, [&](const ClassEntry& entry) {
        guard("index", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const ir::ClassUnit&   unit = session->unit();
            const std::string_view self = session->backing().thisName();
            for (ca::usize i = 0; i < unit.methods.size(); ++i) {
                const ir::MethodUnit&  mu   = unit.methods[i];
                const std::string_view name = unit.symbols().str(mu.name);
                const std::string_view desc = unit.symbols().str(mu.desc);
                if (dup == IndexDup::KeepFirst) {
                    index.emplace(keyOf(self, name, desc),
                                  MethodLoc{std::string(self), i, mu.access});
                } else {
                    index.insert_or_assign(keyOf(self, name, desc),
                                           MethodLoc{std::string(self), i, mu.access});
                }
            }
        });
    });
}

/// @brief DOM 序上从 `from` 起找第一条非 NOP 指令（NOP 跳过，squeeze 遗留
///        落点不驻栈；number_fold / string_pool_fold 同口径）。跨度内任何
///        指令带标签即置 `*labeledFlag`（合并点 = 外部到达路径栈形状不可知，
///        调用方 fail-closed）。@return 指令指针；越序尾/悬空返回 nullptr。
inline const jbc::ir::Insn* nextRealInsn(jbc::ir::CodeDom&              dom,
                                         const std::vector<jbc::ir::InsnId>& order,
                                         ca::usize from, ca::usize* outQ, bool* labeledFlag)
{
    for (ca::usize q = from; q < order.size(); ++q) {
        const jbc::ir::Insn* in = dom.peek(order[q]);
        if (in == nullptr) return nullptr;
        if (!dom.labelsAt(order[q]).empty()) *labeledFlag = true;
        if (in->op != jbc::core::Op::NOP) {
            *outQ = q;
            return in;
        }
    }
    return nullptr;
}

/// @brief 池求值面的 JDK 纯函数 mock 安装（dead_member 池明文提取与
///        pool_array_fold 数组终态求值共用，原 dead_member 私有副本单源）。
///
/// 两面覆盖池填充走廊的 JDK 调用：`parseUnsignedLong`（池解密器 seed 走廊）
/// 与 `toCharArray`（literal 密文面进解密器）。两者全序纯函数，常量实参下
/// 值唯一确定，接管不引入猜测面；坏数字/溢出与 NumberFormatException 同桶
/// Bail（jsim v1 无异常派发）。调用方按需另装 `jsim::str` / `jsim::num`
/// 扩展包（分层红线：本工具只接管这两个 v1 未覆盖面，不替调用方装包）。
inline void installPoolEvalMocks(jsim::Session& session)
{
    session.callMocks["java/lang/Long.parseUnsignedLong(Ljava/lang/String;I)J"] =
        [](jsim::Frame& frame) -> jsim::Result<jsim::SimValue, jsim::Bail> {
        auto radix = frame.popChecked();
        if (!radix.isOk()) return radix;
        auto text = frame.popChecked();
        if (!text.isOk()) return text;
        if (!text.value().known || text.value().type != jsim::ValType::Ref
            || text.value().ref.kind != jsim::RefPayload::Kind::Str
            || !radix.value().known) {
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                jsim::SimValue::unknownOf(jsim::ValType::I64));
        }
        const ca::i32 radixV = static_cast<ca::i32>(radix.value().asU32());
        if (radixV < 2 || radixV > 36) {
            return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                {jsim::Bail::Reason::NotConcrete, frame.pc(),
                 "parseUnsignedLong radix 越界（NumberFormatException v1 不做）"});
        }
        std::vector<ca::u16> units;
        if (!mutf8ToUnits(text.value().ref.str, units)) {
            return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                {jsim::Bail::Reason::NotConcrete, frame.pc(),
                 "parseUnsignedLong 输入串 MUTF-8 解码失败"});
        }
        ca::u64 acc = 0;
        for (const ca::u16 u : units) {
            ca::i32 d = -1;
            if (u >= '0' && u <= '9') d = static_cast<ca::i32>(u - '0');
            else if (u >= 'a' && u <= 'z') d = static_cast<ca::i32>(u - 'a') + 10;
            else if (u >= 'A' && u <= 'Z') d = static_cast<ca::i32>(u - 'A') + 10;
            if (d < 0 || d >= radixV) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                    {jsim::Bail::Reason::NotConcrete, frame.pc(),
                     "parseUnsignedLong 坏数字（NumberFormatException v1 不做）"});
            }
            const ca::u64 next = acc * static_cast<ca::u64>(radixV)
                                 + static_cast<ca::u64>(d);
            if (next < acc) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                    {jsim::Bail::Reason::NotConcrete, frame.pc(),
                     "parseUnsignedLong 溢出（NumberFormatException v1 不做）"});
            }
            acc = next;
        }
        return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
            jsim::SimValue::knownI64(static_cast<ca::i64>(acc)));
    };
    session.callMocks["java/lang/String.toCharArray()[C"] =
        [](jsim::Frame& frame) -> jsim::Result<jsim::SimValue, jsim::Bail> {
        auto recv = frame.popChecked();
        if (!recv.isOk()) return recv;
        if (!recv.value().known || recv.value().type != jsim::ValType::Ref
            || recv.value().ref.kind != jsim::RefPayload::Kind::Str) {
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                jsim::SimValue::unknownOf(jsim::ValType::Ref));
        }
        std::vector<ca::u16> units;
        if (!mutf8ToUnits(recv.value().ref.str, units)) {
            return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                {jsim::Bail::Reason::NotConcrete, frame.pc(),
                 "toCharArray 输入串 MUTF-8 解码失败"});
        }
        jsim::HeapObj arr;
        arr.className = "[C";
        arr.elems.reserve(units.size());   // 原始类型数组载荷在 elems（位型解释）
        for (const ca::u16 u : units) {
            arr.elems.push_back(static_cast<ca::i64>(u));
        }
        const ca::u64 id = frame.session().nextAllocId++;
        frame.session().heap[id] = std::move(arr);
        return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
            jsim::SimValue::knownObj(id));
    };
}

}  // namespace grunt::scan_tools
