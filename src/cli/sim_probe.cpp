/// @file        sim_probe.cpp
/// @brief       CLI 探针实现：容器 → ClassSession → jsim::Method → 跑一次。
///
/// 探针的判据是「行为可复现」：同一个方法、同一组输入，模拟执行的结果必须
/// 和真机一致。它不负责改写，也不参与 pass 链。

#include "cli/sim_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "grunt/container.hpp"
#include "grunt/jsim_bridge.hpp"

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

}   // namespace

int runSimProbe(const std::string& jarPath, const std::string& target,
                const std::string& argsSpec)
{
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
            found = i;
            break;
        }
    }
    if (found == jbc::ir::kNoMethod) {
        std::fprintf(stderr, "方法不存在：%s\n", methodSpec.c_str());
        return 1;
    }

    jsim::Method simMethod;
    std::string  why;
    if (!jsim_bridge::toSimMethod(*session, found, simMethod, &why)) {
        std::fprintf(stderr, "桥转换失败：%s\n", why.c_str());
        return 1;
    }

    std::printf("方法 %s%s，%zu 条指令，maxLocals=%u\n", simMethod.name.c_str(),
                simMethod.desc.c_str(), simMethod.code.size(),
                static_cast<unsigned>(simMethod.maxLocals));

    // 预填：参数按描述符声明类型灌值，其余槽位留 Unknown。
    const std::vector<jsim::ValType>        types = paramTypes(simMethod.desc);
    const std::vector<std::string>          argTexts = splitBy(argsSpec, ',');
    jsim::Prefill                           prefill;
    prefill.locals.reserve(simMethod.maxLocals);
    for (jsim::usize i = 0; i < simMethod.maxLocals; ++i) {
        prefill.locals.push_back(jsim::SimValue::unknownOf(jsim::ValType::I32));
    }
    ca::u32 slot = 0;
    for (jsim::usize i = 0; i < types.size() && i < argTexts.size(); ++i) {
        prefill.locals[slot] = makeArg(argTexts[i], types[i]);
        slot += (types[i] == jsim::ValType::I64 || types[i] == jsim::ValType::F64) ? 2 : 1;
    }

    jsim::Session simSession;
    jsim::str::install(simSession);
    jsim::num::install(simSession);
    if (std::getenv("GRUNT_SIM_TRACE") != nullptr) {
        simSession.trace = [](jsim::u32 pc, const char* opcode, const std::string& stackText) {
            std::printf("  [pc %3u] %-14s %s\n", pc, opcode, stackText.c_str());
        };
    }

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
