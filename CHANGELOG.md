# Changelog

所有显著改动记录在本文件。格式参照 Keep a Changelog；版本号遵循语义化版本。

## 0.1.0（2026-11-12）

首个发布版本：Grunt 家族（Gruntpocalypse / Grunteon）混淆 jar 的静态还原工具，mjbase pass-pack（24 个 pass）+ 独立 CLI，三段骨架（scan / rewrite / proof）。

### 还原面

- 值还原：`TrampolineSqueeze`（蹦床 goto）、`NumberFold`（藏形数值跨度）、`StringPoolFold`（字符串池读点，v2/v3）、`ConstMethod`（常量方法内联）、`ConstValuePromote`（`ConstantValue` 回填）、`SeedStrip`（clinit 种子建表跨度）、`MaterialKeyRestore`（RuntimeMaterial 材料键折回）。
- 调用点还原：`ConstantCall`（全常量实参折叠）、`ProxyInline` / `FieldAccessProxy`（static 方法/字段代理）、`IndyFold`（jsim 模拟执行 BSM）、`IndyRestore`（Grunt indy 直接解密：simple/heavy + RuntimeMaterial 材料通道）、`IndyDecode` / `StringCacheFold` / `MetadataStrip`（Kasumi 专链三面）、`DispatcherRestore`（InvokeDispatcher 调用点回填）、`GateProcessorFold`（gate processor 折叠）。
- 控制流：`PredicateFold`（常量分支/switch 裁决）、`DeadCode`（不可达指令与异常表收敛）、`ExcBridgeRestore`（异常桥还原）、`CffUnflatten`（控制流摊平反演）。
- 清理：`DeadMember`（零引用成员与引用闭包）、`FlagsStrip`（伪 synthetic/bridge 标志）、`BoogerAnnoStrip`（残留 @Booger 注解）。

### 已验证链序

- BoogerV2 主线 15 pass（见 README 链序）。
- v3 供给链 `IndyRestore → StringPoolFold → MaterialKeyRestore → IndyRestore`（boar 语料 materialLane 42228 → 5060）。
- v3 块序与调用点段 `ConstantCall → ExcBridgeRestore → GateProcessorFold → CffUnflatten → DispatcherRestore`。
- Kasumi 专链 `IndyDecode → StringCacheFold → MetadataStrip`（端到端五项检查见 doc/kasumi-verify.rst）。

### 已知边界

- gate 算术随 native 管线移出 jar 时（如 `keyProcessorNativeCandidateRatio=1.0`），恢复目标是无方法体的 native 壳，静态面上无解，需要 native 库才能前进一步。
- SSA exp 摊平变体、运行期随机检查（ThreadLocalRandom 一类）不在处理范围。
- v2（Gruntpocalypse）中频面尚未处理：indy heavy 形、ConstPoolEncrypt 调用点回填、ConstBuilder、ArithmeticEncrypt 恒等简化、MethodExtractor、StringEqualsRedirect、垃圾类、Controlflow 假跳转/陷进环、NumberEncrypt arrayed 池；明细见 doc/obf-behaviors.rst 缺口清单。
- Class/Method/Field 改名不在还原面内，走外部 mapping 旁证。
- 产物面破坏开关（corruptCRC32 / corruptHeaders）与检测类面不做还原处理。

### 工程面

- 容器级不变量：类数不增、输入 jar 只读、fail-closed（判据不全整站点跳过并记拒绝原因）、改写粒度为类。
- CI：文档检查（引用校验 + Sphinx -W + Doxygen）与 Linux/Windows 构建测试；`CffUnflatten` / `ExcBridgeRestore` 两 pass 另加 JVM 实跑检查。
- 语料回归（test/corpus-regress.sh）判据是输入与产物各自过一遍校验器，失败集合必须相同，不依赖黄金字节。
- 基座 mjbase 0.0.18（luiox-repo 包消费）；改写管线内存流式化，v3 全链内存 ~6.4GB 降至 ~1.7GB。
- 单测夹具一律 jasm 现场汇编，不落二进制、不依赖语料目录。
- 文档两层：形态文档在 doc/（每残留一篇），实现说明在各 pass 源码文件头注释。
