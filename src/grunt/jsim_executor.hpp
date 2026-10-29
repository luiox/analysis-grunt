/// @file        jsim_executor.hpp
/// @brief       跨方法执行器：单方法引擎之上的编排层。
///
/// 引擎按设计 D9 只跑一个方法、不递归。但真实解密链不是单方法：
/// `<clinit>` 建池 → decrypt 读池 → decrypt 里又 `invokestatic` 另一个解密
/// 方法（Grunt 的 BSM 就是这么调的）。这条链只能由编排层把「调用」接住、
/// 再开一个引擎实例跑下去——**引擎不递归，编排递归**。
///
/// 接法用 Session 的兜底解析口（callMockResolver）：认领「容器内、有码、
/// 不是 <init>/<clinit>」的调用，弹实参、跑子方法、压回返回值。认不出就放行，
/// 交回核心 Bail——不把缺环境吞成 Unknown。
///
/// 三条纪律：
/// 1. **同一个 Session**：堆与静态字段跨方法共享，`clinit` 建的池子才能被
///    decrypt 读到（这正是「接力棒」的语义）。
/// 2. **深度与步数预算**：解密方法之间可能互相调用，没有闸门就是栈溢出。
/// 3. **失败即失败**：子方法 Bail 直接向上传，不返回半截值。
///
/// 分层红线：本文件**只依赖 jsim 核心**，不认识任何扩展包。String 怎么拼、
/// Integer 怎么解析，是调用方的事——`exec.session()` 交出去，谁需要谁装：
///
///     SimExecutor exec(container);
///     jsim::str::install(exec.session());     // 需要 String 才装
///     jsim::num::install(exec.session());     // 需要 Integer 才装
///
/// 编排层如果替调用方装包，就等于把扩展包钉进了框架——那样「可选」是假的。
#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "grunt/container.hpp"
#include "grunt/jsim_bridge.hpp"

#include "jsim/engine.hpp"

namespace grunt {

/// @brief 一次执行的结局。
struct SimOutcome
{
    enum class Kind : ca::u8 {
        Returned,
        Void,
        Bailed,
        LimitHit,
    };

    Kind          kind = Kind::Bailed;
    jsim::SimValue value;
    std::string   detail;
    ca::u64       steps = 0;

    bool ok() const noexcept { return kind == Kind::Returned || kind == Kind::Void; }
};

class SimExecutor
{
public:
    struct Limits
    {
        ca::u32 maxDepth         = 6;
        ca::u64 maxStepsPerCall = 200000;
        ca::u64 maxTotalSteps    = 4000000;
        ca::usize maxResultCache = 8192;
    };

    explicit SimExecutor(const mjbase::model::ResourceContainer& container, Limits limits = {})
        : container_(&container), limits_(limits)
    {
        // 只挂编排钩子，不装任何扩展包（见文件头分层红线）。
        // 走解析链而不是单槽：反射包等扩展也要这个口子，单槽会互相顶掉。
        session_.callMockResolvers.push_back(
            [this](const jsim::CallRef& callee, jsim::Frame& frame) {
                return resolve(callee, frame);
            });
    }

    jsim::Session&       session() noexcept { return session_; }
    const jsim::Session& session() const noexcept { return session_; }

    /// @brief 已执行的调用数 / 缓存命中数 / 累计步数（观测面）。
    struct Stats
    {
        ca::u64 calls      = 0;
        ca::u64 cacheHits  = 0;
        ca::u64 totalSteps = 0;
        ca::u64 bails      = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

    /// @brief 跑一个方法。`classUri` 可省 `.class`；`method` 支持名字 / `名字+描述符` / 下标。
    SimOutcome invoke(const std::string& classUri, const std::string& method,
                      const std::vector<jsim::SimValue>& args)
    {
        std::string uri = classUri;
        if (uri.size() < 6 || uri.compare(uri.size() - 6, 6, ".class") != 0) uri += ".class";
        const std::vector<ca::u8>* bytes = findClassBytes(*container_, uri);
        if (bytes == nullptr) return fail("类不存在：" + uri);
        std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);
        if (!s) return fail("类解析失败：" + uri);

        const ca::usize index = findMethod(*s, method);
        if (index == jbc::ir::kNoMethod) return fail("方法不存在：" + method);

        const jsim::Method* code = buildMethod(*s, index);
        if (code == nullptr) return fail("桥转换失败：" + method);

        jsim::Prefill prefill;
        prefill.locals.assign(code->maxLocals, jsim::SimValue::unknownOf(jsim::ValType::I32));
        ca::u32 slot = 0;
        for (ca::usize i = 0; i < args.size() && slot < prefill.locals.size(); ++i) {
            prefill.locals[slot] = args[i];
            slot += (args[i].isCat2()) ? 2 : 1;
        }
        return run(*code, std::move(prefill), 0);
    }

private:
    // ── 解析与缓存 ─────────────────────────────────────────────────────────

    static std::string keyOf(const std::string& owner, const std::string& name,
                             const std::string& desc)
    {
        return owner + "." + name + desc;
    }

    static ca::usize findMethod(jbc::ir::ClassSession& s, const std::string& spec)
    {
        const jbc::ir::ClassUnit& unit = s.unit();
        const bool byIndex =
            !spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos;
        if (byIndex) {
            const unsigned long idx = std::strtoul(spec.c_str(), nullptr, 10);
            if (idx < unit.methods.size() && jbc::ir::hasCode(s, static_cast<ca::usize>(idx))) {
                return static_cast<ca::usize>(idx);
            }
            return jbc::ir::kNoMethod;
        }
        std::string wantName = spec;
        std::string wantDesc;
        if (const std::size_t paren = spec.find('('); paren != std::string::npos) {
            wantName = spec.substr(0, paren);
            wantDesc = spec.substr(paren);
        }
        for (ca::usize i = 0; i < unit.methods.size(); ++i) {
            const jbc::ir::MethodUnit& mu = unit.methods[i];
            if (unit.symbols().str(mu.name) != wantName) continue;
            if (!wantDesc.empty() && unit.symbols().str(mu.desc) != wantDesc) continue;
            if (!jbc::ir::hasCode(s, i)) continue;
            return i;
        }
        return jbc::ir::kNoMethod;
    }

    /// @brief 取（或建）一个方法的模拟视图。方法体自持字符串，会话可随即释放。
    const jsim::Method* buildMethod(jbc::ir::ClassSession& s, ca::usize index)
    {
        const jbc::ir::ClassUnit&  unit = s.unit();
        const jbc::ir::MethodUnit& mu   = unit.methods[index];
        const std::string owner = std::string(unit.symbols().str(unit.thisClass));
        const std::string key   = keyOf(owner, std::string(unit.symbols().str(mu.name)),
                                        std::string(unit.symbols().str(mu.desc)));

        auto it = methods_.find(key);
        if (it != methods_.end()) {
            return it->second.ok ? &it->second.method : nullptr;
        }
        Slot slot;
        std::string why;
        slot.ok = jsim_bridge::toSimMethod(s, index, slot.method, &why);
        slot.why = std::move(why);
        auto inserted = methods_.emplace(key, std::move(slot)).first;
        return inserted->second.ok ? &inserted->second.method : nullptr;
    }

    /// @brief 按 owner 打开类并取方法；失败返回 nullptr（不猜）。
    const jsim::Method* methodOf(const std::string& owner, const std::string& name,
                                 const std::string& desc)
    {
        const std::string key = keyOf(owner, name, desc);
        auto             it   = methods_.find(key);
        if (it != methods_.end()) {
            return it->second.ok ? &it->second.method : nullptr;
        }

        std::string uri = owner + ".class";
        const std::vector<ca::u8>* bytes = findClassBytes(*container_, uri);
        if (bytes == nullptr) return nullptr;
        std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);
        if (!s) return nullptr;

        const ca::usize index = findMethod(*s, name + desc);
        if (index == jbc::ir::kNoMethod) return nullptr;

        Slot slot;
        std::string why;
        slot.ok = jsim_bridge::toSimMethod(*s, index, slot.method, &why);
        slot.why = std::move(why);
        auto inserted = methods_.emplace(key, std::move(slot)).first;
        return inserted->second.ok ? &inserted->second.method : nullptr;
    }

    // ── 调用接管 ───────────────────────────────────────────────────────────

    /// @brief 认领条件：容器内有这个类、这个方法、且有码。
    ///
    /// `<init>`/`<clinit>` 不认领：跑构造器等于把对象状态编一半，跑类初始化
    /// 会把 clinit 的副作用提前——两者都不是「求一个返回值」的语义。
    jsim::CallMock* resolve(const jsim::CallRef& callee, jsim::Frame& frame)
    {
        (void)frame;
        if (depth_ >= limits_.maxDepth) return nullptr;
        if (callee.name == "<init>" || callee.name == "<clinit>") return nullptr;
        if (callee.owner.empty() || callee.owner[0] == '<') return nullptr;

        const std::string key = keyOf(callee.owner, callee.name, callee.desc);
        auto              it  = mocks_.find(key);
        if (it != mocks_.end()) return it->second ? &it->second.value() : nullptr;

        const jsim::Method* target = methodOf(callee.owner, callee.name, callee.desc);
        if (target == nullptr) {
            mocks_.emplace(key, std::nullopt);   // 记下「不可认领」，避免重复解析
            return nullptr;
        }

        std::vector<jsim::ValType> params;
        if (!paramTypes(callee.desc, params)) {
            mocks_.emplace(key, std::nullopt);
            return nullptr;
        }
        const bool     isStatic = callee.isStatic;
        jsim::CallMock mock = [this, key, params, isStatic](jsim::Frame& f) {
            std::vector<jsim::SimValue> args(params.size(),
                                             jsim::SimValue::unknownOf(jsim::ValType::I32));
            for (ca::usize i = params.size(); i-- > 0;) {
                auto popped = f.popChecked();
                if (!popped.isOk()) return jsim::Result<jsim::SimValue, jsim::Bail>::err(popped.error());
                args[i] = popped.value();
            }
            if (!isStatic) {
                auto recv = f.popChecked();
                if (!recv.isOk()) return jsim::Result<jsim::SimValue, jsim::Bail>::err(recv.error());
            }
            SimOutcome out = invokeByKey(key, args, f.pc());
            if (!out.ok()) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::err(
                    {jsim::Bail::Reason::UnsupportedCall, f.pc(),
                     "子方法未收敛：" + out.detail});
            }
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(out.value);
        };
        auto inserted = mocks_.emplace(key, std::move(mock)).first;
        return inserted->second ? &inserted->second.value() : nullptr;
    }

    /// @brief 描述符 → 参数类型序列（含 `[`/`L...;` 两种引用形态）。
    static bool paramTypes(const std::string& desc, std::vector<jsim::ValType>& out)
    {
        std::size_t i = desc.find('(');
        if (i == std::string::npos) return false;
        ++i;
        while (i < desc.size() && desc[i] != ')') {
            const char c = desc[i];
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
            if (!jsim_bridge::valTypeOfDescChar(c, t)) return false;
            out.push_back(t);
            ++i;
        }
        return i < desc.size();
    }

    // ── 执行 ───────────────────────────────────────────────────────────────

    SimOutcome invokeByKey(const std::string& key, const std::vector<jsim::SimValue>& args,
                           ca::u32 callPc)
    {
        auto it = methods_.find(key);
        if (it == methods_.end() || !it->second.ok) {
            return fail("方法不可分析：" + key);
        }
        const jsim::Method& code = it->second.method;

        // 结果缓存：只对「参数全 Known 且不含堆对象」的调用缓存——objId 是会话
        // 内的单调序号，拿它当键会命中错对象。
        const std::string cacheKey = makeCacheKey(key, args);
        if (!cacheKey.empty()) {
            auto hit = results_.find(cacheKey);
            if (hit != results_.end()) {
                ++stats_.cacheHits;
                return hit->second;
            }
        }

        jsim::Prefill prefill;
        prefill.locals.assign(code.maxLocals, jsim::SimValue::unknownOf(jsim::ValType::I32));
        ca::u32 slot = 0;
        for (ca::usize i = 0; i < args.size() && slot < prefill.locals.size(); ++i) {
            prefill.locals[slot] = args[i];
            slot += (args[i].isCat2()) ? 2 : 1;
        }

        SimOutcome out = run(code, std::move(prefill), callPc);
        if (!cacheKey.empty() && out.ok()) {
            if (results_.size() < limits_.maxResultCache) results_.emplace(cacheKey, out);
        }
        return out;
    }

    SimOutcome run(const jsim::Method& code, jsim::Prefill prefill, ca::u32 callPc)
    {
        if (stats_.totalSteps >= limits_.maxTotalSteps) {
            return fail("总步数预算耗尽");
        }
        ++depth_;
        ++stats_.calls;
        jsim::Limits limits;
        limits.maxSteps = limits_.maxStepsPerCall;
        const jsim::RunResult r =
            jsim::Sim(code, session_).run(0, std::move(prefill), limits);
        --depth_;
        stats_.totalSteps += r.steps;

        SimOutcome out;
        out.steps = r.steps;
        switch (r.kind) {
        case jsim::RunResult::Kind::Completed:
            out.kind = r.hasReturnValue ? SimOutcome::Kind::Returned : SimOutcome::Kind::Void;
            out.value = r.returnValue;
            return out;
        case jsim::RunResult::Kind::Bailed:
            out.kind = SimOutcome::Kind::Bailed;
            out.detail = std::string("Bail@") + std::to_string(r.bail.pc) + "："
                         + r.bail.detail + "（调用点 " + std::to_string(callPc) + "）";
            ++stats_.bails;
            return out;
        case jsim::RunResult::Kind::LimitHit:
            out.kind = SimOutcome::Kind::LimitHit;
            out.detail = "步数熔断（" + std::to_string(r.steps) + " 步）";
            ++stats_.bails;
            return out;
        }
        return out;
    }

    /// @brief 缓存键：方法 + 参数展示形。含 Obj 参数或 Unknown 参数时返回空（不缓存）。
    static std::string makeCacheKey(const std::string& key, const std::vector<jsim::SimValue>& args)
    {
        std::string out = key;
        for (const jsim::SimValue& v : args) {
            if (!v.known) return {};
            if (v.type == jsim::ValType::Ref && v.ref.kind == jsim::RefPayload::Kind::Obj) {
                return {};
            }
            out += '|';
            out += jsim::toDisplay(v);
        }
        return out;
    }

    static SimOutcome fail(std::string detail)
    {
        SimOutcome out;
        out.kind   = SimOutcome::Kind::Bailed;
        out.detail = std::move(detail);
        return out;
    }

    struct Slot
    {
        bool         ok = false;
        jsim::Method method;
        std::string  why;
    };

    const mjbase::model::ResourceContainer* container_ = nullptr;
    Limits                                  limits_{};
    jsim::Session                           session_;
    std::unordered_map<std::string, Slot>   methods_;
    std::unordered_map<std::string, std::optional<jsim::CallMock>> mocks_;
    std::unordered_map<std::string, SimOutcome> results_;
    Stats                                   stats_{};
    ca::u32                                 depth_ = 0;
};

}   // namespace grunt
