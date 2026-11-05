# doc 索引

口径分工：

- `doc/<residue>.md` 只写**混淆器会产出什么结构**——换个输入 jar 仍成立才写。
- `doc/<residue>.impl.md` 只写**本仓怎么还原**——固定三章：Scan / Rewrite / Proof。
- 具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
- 唯一例外：`kasumi-verify.md` 是专链端到端**验收记录**（五项门数字 + 口径差异 + 通用链取证），只记数字与来源，语料 jar / TSV / 产物不进仓。

## 残留

| 文件 | 内容 |
|---|---|
| [constant-call.md](constant-call.md) | 常量调用：形态、为什么是常量、常见变体、与相邻形态的区别 |
| [constant-call.impl.md](constant-call.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [flags-strip.md](flags-strip.md) | 伪 synthetic/bridge 标志：形态、与编译器合成物的区分 |
| [flags-strip.impl.md](flags-strip.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [const-method.md](const-method.md) | 常量方法：形态、基元 `.TYPE` 桥、与常量字段的依赖关系 |
| [const-method.impl.md](const-method.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [trampoline-squeeze.md](trampoline-squeeze.md) | 蹦床：跳到顺序后继的 `goto` 的形态与来源 |
| [trampoline-squeeze.impl.md](trampoline-squeeze.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [predicate-fold.md](predicate-fold.md) | 常量谓词：分支 / `switch` 的静态可裁决形态 |
| [predicate-fold.impl.md](predicate-fold.impl.md) | 本仓的 Scan / Rewrite / Proof，含帧链与链序约束 |
| [dead-code.md](dead-code.md) | 死码：异常表作为隐藏控制流边、端点吸附规则 |
| [dead-code.impl.md](dead-code.impl.md) | 本仓的 Scan / Rewrite / Proof，含异常表感知可达性 |
| [const-value-promote.md](const-value-promote.md) | 常量字段：`clinit` 单次常量写 → `ConstantValue` 的形态 |
| [const-value-promote.impl.md](const-value-promote.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [seed-strip.md](seed-strip.md) | 种子建表：`int[]` 种子跨度的固定形状 |
| [seed-strip.impl.md](seed-strip.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [dead-member.md](dead-member.md) | 死成员：零引用方法 / 私有字段 / 空 `<clinit>` 的形态 |
| [dead-member.impl.md](dead-member.impl.md) | 本仓的 Scan / Rewrite / Proof，含压池后算引用闭包 |
| [booger-anno-strip.md](booger-anno-strip.md) | 残留 `@Booger` 注解：解密体被折成常量后留下的空壳 |
| [booger-anno-strip.impl.md](booger-anno-strip.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [number-fold.md](number-fold.md) | 藏形数值常量：XOR 隐匿跨度、基 32 长整跨度、位形包装的形态 |
| [number-fold.impl.md](number-fold.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [indy-fold.md](indy-fold.md) | indy 解密调用点：BSM 藏目标、模拟执行求真目标的形态 |
| [indy-fold.impl.md](indy-fold.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [proxy-inline.md](proxy-inline.md) | 代理方法：恒等转发体的形态与初始化时序约束 |
| [proxy-inline.impl.md](proxy-inline.impl.md) | 本仓的 Scan / Rewrite / Proof 与已知边界 |
| [indy-decode.md](indy-decode.md) | Kasumi indy 静态还原：全量 indy 化调用点、XOR key 链、7 形态表的形态 |
| [indy-decode.impl.md](indy-decode.impl.md) | 本仓的 Scan / Rewrite / Proof，含注入面、插入式锚保留与语料账本 |
| [string-cache-fold.md](string-cache-fold.md) | Kasumi 字符串缓存：读缓存/直接解密两形态调用点、TSV 旁证路线的形态 |
| [string-cache-fold.impl.md](string-cache-fold.impl.md) | 本仓的 Scan / Rewrite / Proof，含 `--tsv` 注入面、旁证降级与语料账本 |
| [metadata-strip.md](metadata-strip.md) | Kasumi 元数据残留：`@Metadata` 注解、水印字段、反 LLM 注入资源的形态 |
| [metadata-strip.impl.md](metadata-strip.impl.md) | 本仓的 Scan / Rewrite / Proof，含资源删除动词、资源对账门与语料账本 |
| [string-pool-fold.md](string-pool-fold.md) | Grunt v2/v3 字符串池：池读三元组、池链三件套、自逆 XOR 流静态复算的形态 |
| [string-pool-fold.impl.md](string-pool-fold.impl.md) | 本仓的 Scan / Rewrite / Proof，含算法源锚点、蹦床跟随与语料账本 |
| [kasumi-verify.md](kasumi-verify.md) | Kasumi 专链端到端复现记录：五项门逐项数字、与树版口径差异点、通用链取证 |

## obf 行为账

Grunt 家族逐混淆行为的描述文档（形态 + 配置面 + jasm 样例 + residue 现状）在 [obf-behaviors/](obf-behaviors/)：

- [obf-behaviors/README.md](obf-behaviors/README.md)——行为 × residue 状态总表与闭环缺口清单（P1/P2/P3）。
- [obf-behaviors/grunt-v2-behaviors.md](obf-behaviors/grunt-v2-behaviors.md)——v2（Gruntpocalypse 2.x）35 个 transformer 行为账。
- [obf-behaviors/grunt-v3-behaviors.md](obf-behaviors/grunt-v3-behaviors.md)——v3（Grunteon）39 个 transformer + GlslObfuscator 插件 + NativePipeline 阶段行为账。

与 morpher 仓 `mjt-deobf/doc_ai/obf-forms/` 的分工：那边是跨家族形态库（artifact 侧形态判据与还原链序），本目录行为账是本工具的逐行为现状与缺口清单，同面判据以那边为准。

## 约定

代码组织约定见仓根 [CONVENTION.md](../CONVENTION.md)。
