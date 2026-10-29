/// @file        sim_probe.cpp
/// @brief       CLI 探针实现：容器 → ClassSession → jsim::Method → 跑一次。
///
/// 探针的判据是「行为可复现」：同一个方法、同一组输入，模拟执行的结果必须
/// 和真机一致。它不负责改写，也不参与 pass 链。
///
/// 两条通路，**显式二选一**：
///
/// - 默认：单方法裸跑。只经过桥 + 引擎，用来证明「真 CodeUnit 转出来的方法」
///   行为正确。子调用一律 Bail——这正是单方法引擎的边界（设计 D9）。
/// - `--exec`：跨方法执行器。子调用被编排层接住、再开引擎实例跑下去，
///   用来证明「clinit 建池 → decrypt 读池 → decrypt 里再调 decrypt」这条链。
///
/// 两路必须分开报告，不能混：如果以为在验桥、其实跑的是执行器，结论就是错的。

#include "cli/sim_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "grunt/container.hpp"
#include "grunt/jsim_bridge.hpp"
#include "grunt/jsim_executor.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_helper.hpp"

#include "jsim/engine.hpp"
#include "jsim/num.hpp"
#include "jsim/str.hpp"

namespace grunt::cli {
namespace {

const char* reasonName(jsim::Bail::Reason r)
{
    switch (r) {
    case jsim::Bail::Reason::UnsupportedOpcode: return "UnsupportedOpcode";
    case jsim::Bail::Reason::UnsupportedCall: return "UnsupportedCall";
    case jsim::Bail::Reason::UnsupportedAlloc: return "UnsupportedAlloc";
    case jsim::Bail::Reason::NotConcrete: return "NotConcrete";
    case jsim::Bail::Reason::BadOperand: return "BadOperand";
    case jsim::Bail::Reason::StackBroken: return "StackBroken";
    case jsim::Bail::Reason::OffMethodEnd: return "OffMethodEnd";
    }
    return "?";
}

/// @brief 描述符参数列表 → 值类型序列。
std::vector<jsim::ValType> paramTypes(std::string_view desc)
{
    std::vector<jsim::ValType> out;
    std::size_t i = desc.find('(');
    if (i == std::string_view::npos) return out;
    ++i;
    while (i < desc.size() && desc[i] != ')') {
        char c = desc[i];
        if (c == '[') {
            while (i < desc.size() && desc[i] == '[') ++i;
            if (i < desc.size() && desc[i] == 'L') {
                while (i < desc.size() && desc[i] != ';') ++i;
            }
            ++i;
            out.push_back(jsim::ValType::Ref);
            continue;
        }
        if (c == 'L') {
            while (i < desc.size() && desc[i] != ';') ++i;
            ++i;
            out.push_back(jsim::ValType::Ref);
            continue;
        }
        jsim::ValType t = jsim::ValType::I32;
        (void)jsim_bridge::valTypeOfDescChar(c, t);
        out.push_back(t);
        ++i;
    }
    return out;
}

std::vector<std::string> splitBy(const std::string& text, char sep)
{
    std::vector<std::string> out;
    std::string              cur;
    for (char ch : text) {
        if (ch == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// @brief 实参文本 → SimValue。`u:` 显式 Unknown，`s:` 字符串，`x:` 十六进制字节。
///
/// `x:` 是必需的：密文是非 ASCII 码元，走 Windows argv 会被 ANSI 代码页改写，
/// 只有十六进制形态能无损穿过命令行。
jsim::SimValue makeArg(const std::string& spec, jsim::ValType type)
{
    if (spec.rfind("u:", 0) == 0) return jsim::SimValue::unknownOf(type);
    if (spec.rfind("s:", 0) == 0) return jsim::SimValue::knownStr(spec.substr(2));
    if (spec.rfind("x:", 0) == 0) {
        std::string bytes;
        const std::string& hex = spec;
        for (std::size_t i = 2; i + 1 < hex.size(); i += 2) {
            bytes += static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        }
        return jsim::SimValue::knownStr(std::move(bytes));
    }
    switch (type) {
    case jsim::ValType::I32:
        return jsim::SimValue::knownI32(static_cast<jsim::i32>(std::strtoll(spec.c_str(), nullptr, 10)));
    case jsim::ValType::I64:
        return jsim::SimValue::knownI64(std::strtoll(spec.c_str(), nullptr, 10));
    case jsim::ValType::F32:
        return jsim::SimValue::knownF32(static_cast<float>(std::strtod(spec.c_str(), nullptr)));
    case jsim::ValType::F64:
        return jsim::SimValue::knownF64(std::strtod(spec.c_str(), nullptr));
    case jsim::ValType::Ref:
        return jsim::SimValue::unknownOf(jsim::ValType::Ref);
    }
    return jsim::SimValue::unknownOf(type);
}

/// @brief 逐步栈形观测。探针与环境变量都不进框架，只在这里挂。
void installTrace(jsim::Session& session)
{
    if (std::getenv("GRUNT_SIM_TRACE") == nullptr) return;
    session.trace = [](jsim::u32 pc, const char* opcode, const std::string& stackText) {
        std::printf("  [pc %3u] %-14s %s\n", pc, opcode, stackText.c_str());
    };
}

/// @brief 把连续重复的 Jasm 空白转义压成 `\u0020×N`。
///
/// Grunt 的方法名是三万个空格，Jasm 按码点转义成三万个 `\u0020`，一行就 200KB。
/// 压成计数后信息不丢、行还能读——否则 --dump 的输出没法看。
std::string collapseEscapes(const std::string& text)
{
    static const std::string kEsc = "\\u0020";
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, kEsc.size(), kEsc) == 0) {
            std::size_t n = 0;
            while (text.compare(i, kEsc.size(), kEsc) == 0) {
                i += kEsc.size();
                ++n;
            }
            if (n == 1) {
                out += kEsc;
            } else {
                out += "\\u0020×" + std::to_string(n);
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

/// @brief 用 mjbase 的 Jasm 门面反汇编单个方法。
///
/// 不自己搓反汇编：mjbase 已经把 libjhasm 包成 `JvmJasmService`，输出是规范化
/// Jasm（常量池、跳转标签、栈图都在），信息量比自制的「pc + 操作数」视图大得多，
/// 而且与回写用的 `assemble` 是同一套口径——改写前后各 dump 一次可以逐行对比。
///
/// 另一层好处：dump 直接吃**原始类字节**，不经过桥。桥转换失败时照样能看到
/// 方法体，排查「是桥的问题还是引擎的问题」才有依据。
bool dumpMethodWithJasm(const std::vector<ca::u8>& classBytes, const std::string& name,
                        const std::string& desc)
{
    mjbase::jasm::JvmJasmService service;
    mjbase::jasm::Result<mjbase::jasm::JvmJasmMethodDisassembly> r = service.disassembleMethod(
        classBytes, ca::str::Utf8StringRef::from_string_view(name),
        ca::str::Utf8StringRef::from_string_view(desc));
    if (!r.isOk()) {
        std::fprintf(stderr, "Jasm 反汇编失败：%s%s\n", name.c_str(), desc.c_str());
        for (const mjbase::jasm::Diagnostic& d : r.diagnostics) {
            std::fprintf(stderr, "  [%s] %s\n",
                         d.severity == mjbase::jasm::DiagnosticSeverity::Error ? "error" : "warn",
                         d.message.to_utf8_string().to_std_string().c_str());
        }
        return false;
    }
    std::printf("%s\n",
                collapseEscapes(r.value->source.to_utf8_string().to_std_string()).c_str());
    return true;
}

/// @brief 桥产物的指令序列（只有 opcode，没有操作数）。
///
/// 这不是反汇编器——反汇编是 Jasm 的活。这里只做**对表**：Jasm 给出的指令数与
/// 桥给出的条数不一致，就说明 `promoteMethod` 的产物和字节码原貌有出入，
/// 而桥是按前者建 pc 的。没有这一行，pc 偏移错位会被当成引擎的行为差异。
void printBridgeShape(const jsim::Method& m)
{
    std::printf("── 桥视图：%zu 条（与上面 Jasm 对表用）\n", m.code.size());
    for (jsim::usize pc = 0; pc < m.code.size(); ++pc) {
        if (pc % 6 == 0) std::printf("  ");
        std::printf("%zu:%s ", pc, jsim::opcodeName(m.code[pc].opcode));
        if (pc % 6 == 5 || pc + 1 == m.code.size()) std::printf("\n");
    }
}

}   // namespace

int runSimProbe(const SimProbeOptions& options)
{
    const std::string& jarPath    = options.jarPath;
    const std::string& target     = options.target;
    const std::string& argsSpec   = options.argsSpec;
    const bool         executor   = options.executor;

    const std::size_t hash = target.find('#');
    if (hash == std::string::npos) {
        std::fprintf(stderr, "--sim 需要 `类uri#方法名` 形态：%s\n", target.c_str());
        return 1;
    }
    std::string classUri = target.substr(0, hash);
    std::string methodSpec = target.substr(hash + 1);
    if (classUri.size() < 6 || classUri.compare(classUri.size() - 6, 6, ".class") != 0) {
        classUri += ".class";
    }

    mjbase::model::ResourceContainer container;
    if (auto m = mjbase::model::ResourceHelper::importFromJar(container, jarPath); m.is_err()) {
        std::fprintf(stderr, "jar 导入失败：%s\n", jarPath.c_str());
        return 1;
    }

    const std::vector<ca::u8>* bytes = findClassBytes(container, classUri);
    if (bytes == nullptr) {
        // 兜底：Grunt 的类名带 U+200E 之类的不可见字符，命令行传参会把它吞成
        // '?'。按子串找唯一命中，命中多个就全部列出让调用方自己精确化。
        const std::string stem = classUri.substr(0, classUri.size() - 6);
        std::vector<std::string> hits;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (entry.uri.find(stem) != std::string_view::npos) {
                hits.emplace_back(entry.uri);
            }
        });
        if (hits.size() == 1) {
            classUri = hits.front();
            bytes    = findClassBytes(container, classUri);
        } else {
            std::fprintf(stderr, "类不存在：%s（子串命中 %zu 个）\n", classUri.c_str(),
                         hits.size());
            for (const std::string& hit : hits) {
                std::fprintf(stderr, "  %s\n", hit.c_str());
            }
            return 1;
        }
    }
    std::shared_ptr<jbc::ir::ClassSession> session = openSession(*bytes);
    if (!session) {
        std::fprintf(stderr, "类解析失败：%s\n", classUri.c_str());
        return 1;
    }

    // 方法名可带描述符后缀精确化：`foo(II)V`
    std::string wantName = methodSpec;
    std::string wantDesc;
    if (const std::size_t paren = methodSpec.find('('); paren != std::string::npos) {
        wantName = methodSpec.substr(0, paren);
        wantDesc = methodSpec.substr(paren);
    }

    const jbc::ir::ClassUnit& unit = session->unit();

    // `#*` 列出全部带码方法——探针要能自己找到目标，不该靠外部猜方法名。
    // 方法名本身可能是「三万个空格」或带不可见字符，所以 `#<数字>` 直接按
    // 方法下标取，配合 `#*` 的输出来定位。
    if (methodSpec == "*") {
        for (ca::usize i = 0; i < unit.methods.size(); ++i) {
            const jbc::ir::MethodUnit& mu = unit.methods[i];
            if (!jbc::ir::hasCode(*session, i)) continue;
            std::printf("%zu\t%s%s\n", i, std::string(unit.symbols().str(mu.name)).c_str(),
                        std::string(unit.symbols().str(mu.desc)).c_str());
        }
        return 0;
    }
    const bool byIndex = !methodSpec.empty()
                         && methodSpec.find_first_not_of("0123456789") == std::string::npos;

    ca::usize                found = jbc::ir::kNoMethod;
    if (byIndex) {
        const unsigned long idx = std::strtoul(methodSpec.c_str(), nullptr, 10);
        if (idx < unit.methods.size() && jbc::ir::hasCode(*session, static_cast<ca::usize>(idx))) {
            found = static_cast<ca::usize>(idx);
        }
    } else {
        for (ca::usize i = 0; i < unit.methods.size(); ++i) {
            const jbc::ir::MethodUnit& mu = unit.methods[i];
            const std::string_view     name = unit.symbols().str(mu.name);
            const std::string_view     desc = unit.symbols().str(mu.desc);
            if (name != wantName) continue;
            if (!wantDesc.empty() && desc != wantDesc) continue;
            if (!jbc::ir::hasCode(*session, i)) continue;
            found = i;
            break;
        }
    }
    if (found == jbc::ir::kNoMethod) {
        std::fprintf(stderr, "方法不存在：%s\n", methodSpec.c_str());
        return 1;
    }

    const std::string desc(unit.symbols().str(unit.methods[found].desc));
    const std::string name(unit.symbols().str(unit.methods[found].name));

    // dump 直接走 Jasm，不经桥：桥转换失败时照样能看到方法体，才能分清
    // 「桥的问题」还是「引擎的问题」。桥视图跟在后面，供对表。
    jsim::Method simMethod;
    std::string  why;
    const bool   bridged = jsim_bridge::toSimMethod(*session, found, simMethod, &why);
    if (options.dump) {
        (void)dumpMethodWithJasm(*bytes, name, desc);
        if (bridged) {
            printBridgeShape(simMethod);
        } else {
            std::printf("── 桥转换失败：%s\n", why.c_str());
        }
    }
    if (!executor && !bridged) {
        std::fprintf(stderr, "桥转换失败：%s\n", why.c_str());
        return 1;
    }

    // 预填实参：类型由描述符给（静态事实），值由命令行给。两路共用这一份。
    const std::vector<jsim::ValType> types    = paramTypes(desc);
    const std::vector<std::string>   argTexts = splitBy(argsSpec, ',');
    std::vector<jsim::SimValue>      argValues;
    argValues.reserve(types.size());
    for (std::size_t i = 0; i < types.size(); ++i) {
        argValues.push_back(i < argTexts.size() ? makeArg(argTexts[i], types[i])
                                                : jsim::SimValue::unknownOf(types[i]));
    }

    // ── 跨方法执行器 ────────────────────────────────────────────────────────
    //
    // 扩展包由**探针**装，不由执行器装（分层红线）：执行器只认 jsim 核心，
    // String 怎么拼、Integer 怎么解析，是调用方的事。要证明「拿掉扩展包
    // 也能跑」，删掉下面两行即可。
    if (executor) {
        std::printf("方法 %s%s（执行器）\n", name.c_str(), desc.c_str());

        grunt::SimExecutor exec(container);
        jsim::str::install(exec.session());
        jsim::num::install(exec.session());
        installTrace(exec.session());

        const SimOutcome out = exec.invoke(classUri, methodSpec, argValues);
        const SimExecutor::Stats& st = exec.stats();
        switch (out.kind) {
        case SimOutcome::Kind::Returned:
            std::printf("→ Completed：%s（%llu 步）\n", jsim::toDisplay(out.value).c_str(),
                        static_cast<unsigned long long>(out.steps));
            break;
        case SimOutcome::Kind::Void:
            std::printf("→ Completed：void（%llu 步）\n",
                        static_cast<unsigned long long>(out.steps));
            break;
        case SimOutcome::Kind::Bailed:
            std::printf("→ Bailed：%s（%llu 步）\n", out.detail.c_str(),
                        static_cast<unsigned long long>(out.steps));
            break;
        case SimOutcome::Kind::LimitHit:
            std::printf("→ LimitHit：%s（%llu 步）\n", out.detail.c_str(),
                        static_cast<unsigned long long>(out.steps));
            break;
        }
        std::printf("  调用 %llu 次，缓存命中 %llu 次，累计 %llu 步，Bail %llu 次\n",
                    static_cast<unsigned long long>(st.calls),
                    static_cast<unsigned long long>(st.cacheHits),
                    static_cast<unsigned long long>(st.totalSteps),
                    static_cast<unsigned long long>(st.bails));
        return out.ok() ? 0 : 1;
    }

    // ── 单方法裸跑（验桥）──────────────────────────────────────────────────
    std::printf("方法 %s%s，%zu 条指令，maxLocals=%u\n", simMethod.name.c_str(),
                simMethod.desc.c_str(), simMethod.code.size(),
                static_cast<unsigned>(simMethod.maxLocals));

    jsim::Prefill prefill;
    prefill.locals.reserve(simMethod.maxLocals);
    for (jsim::usize i = 0; i < simMethod.maxLocals; ++i) {
        prefill.locals.push_back(jsim::SimValue::unknownOf(jsim::ValType::I32));
    }
    ca::u32 slot = 0;
    for (jsim::usize i = 0; i < argValues.size() && slot < prefill.locals.size(); ++i) {
        prefill.locals[slot] = argValues[i];
        slot += (argValues[i].isCat2()) ? 2 : 1;
    }

    jsim::Session simSession;
    jsim::str::install(simSession);
    jsim::num::install(simSession);
    installTrace(simSession);

    const jsim::RunResult r = jsim::Sim(simMethod, simSession).run(0, std::move(prefill), {100000});
    switch (r.kind) {
    case jsim::RunResult::Kind::Completed:
        std::printf("→ Completed：%s（%llu 步）\n",
                    r.hasReturnValue ? jsim::toDisplay(r.returnValue).c_str() : "void",
                    static_cast<unsigned long long>(r.steps));
        return 0;
    case jsim::RunResult::Kind::Bailed:
        std::printf("→ Bailed[%s] @pc=%u：%s（%llu 步）\n", reasonName(r.bail.reason), r.bail.pc,
                    r.bail.detail.c_str(), static_cast<unsigned long long>(r.steps));
        return 1;
    case jsim::RunResult::Kind::LimitHit:
        std::printf("→ LimitHit：%llu 步熔断\n", static_cast<unsigned long long>(r.steps));
        return 1;
    }
    return 1;
}

}   // namespace grunt::cli
