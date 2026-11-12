# doc 索引

本目录正处 md→rst 迁移期：RST 站（入口 `doc/index.rst`，CI 以 sphinx -W + check_refs + doxygen 门禁）收行为总表 / 语料账本 / 复杂 pass 规格，已迁篇删 md、未迁篇原样保留待后续批。

口径分工：

- `doc/<residue>.md` 只写**混淆器会产出什么结构**——换个输入 jar 仍成立才写。
- **本仓怎么还原**（固定三章：Scan / Rewrite / Proof，含判据分级、改写契约与已知边界）在各 pass 源码文件头注释：`src/grunt/<pass>.cpp` / `<pass>.hpp` 的 `///` 面（Doxygen 门禁覆盖）。
- 具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
- 唯一例外：`kasumi-verify.md` 是专链端到端**验收记录**（五项门数字 + 口径差异 + 通用链取证），只记数字与来源，语料 jar / TSV / 产物不进仓。

## 残留

| 文件 | 内容 |
|---|---|
| [constant-call.md](constant-call.md) | 常量调用：形态、为什么是常量、常见变体、与相邻形态的区别 |
| [flags-strip.md](flags-strip.md) | 伪 synthetic/bridge 标志：形态、与编译器合成物的区分 |
| [const-method.md](const-method.md) | 常量方法：形态、基元 `.TYPE` 桥、与常量字段的依赖关系 |
| [trampoline-squeeze.md](trampoline-squeeze.md) | 蹦床：跳到顺序后继的 `goto` 的形态与来源 |
| [predicate-fold.md](predicate-fold.md) | 常量谓词：分支 / `switch` 的静态可裁决形态 |
| [dead-code.md](dead-code.md) | 死码：异常表作为隐藏控制流边、端点吸附规则 |
| [const-value-promote.md](const-value-promote.md) | 常量字段：`clinit` 单次常量写 → `ConstantValue` 的形态 |
| [seed-strip.md](seed-strip.md) | 种子建表：`int[]` 种子跨度的固定形状 |
| [dead-member.md](dead-member.md) | 死成员：零引用方法 / 私有字段 / 空 `<clinit>` 的形态 |
| [booger-anno-strip.md](booger-anno-strip.md) | 残留 `@Booger` 注解：解密体被折成常量后留下的空壳 |
| [number-fold.md](number-fold.md) | 藏形数值常量：XOR 隐匿跨度、基 32 长整跨度、位形包装的形态 |
| [indy-fold.md](indy-fold.md) | indy 解密调用点：BSM 藏目标、模拟执行求真目标的形态 |
| [proxy-inline.md](proxy-inline.md) | 代理方法：恒等转发体的形态与初始化时序约束 |
| [indy-decode.md](indy-decode.md) | Kasumi indy 静态还原：全量 indy 化调用点、XOR key 链、7 形态表的形态 |
| [string-cache-fold.md](string-cache-fold.md) | Kasumi 字符串缓存：读缓存/直接解密两形态调用点、TSV 旁证路线的形态 |
| [metadata-strip.md](metadata-strip.md) | Kasumi 元数据残留：`@Metadata` 注解、水印字段、反 LLM 注入资源的形态 |
| [string-pool-fold.md](string-pool-fold.md) | Grunt v2/v3 字符串池：池读三元组、池链三件套、自逆 XOR 流静态复算的形态 |
| [dispatcher-restore.md](dispatcher-restore.md) | Grunt v3 InvokeDispatcher：tableswitch 分发器、头锚识别、调用点 key 反查回填的形态 |
| [cff-unflatten.md](cff-unflatten.md) | Grunt v3 ControlflowFlattening：state 槽摊平岛、常量供给桥、alias-face 剪枝反演的形态 |
| [material-key-restore.md](material-key-restore.md) | Grunt v3 RuntimeMaterial：材料通道解密器窗口、materialKey() 逐字 9 条、初值链（基 32 parse 面 + poison 走廊）的形态 |
| [exc-bridge-restore.md](exc-bridge-restore.md) | Grunt v3 ControlflowJump 异常桥：合成 throw/catch 蹦床改道边、形状钉 + 异常表互证的形态 |
| [gate-processor-fold-spec.rst](gate-processor-fold-spec.rst) | （原 gate-processor-fold.md 已并入）Grunt v3 ControlflowJump 门点：适用情况 / 算法 / 前后 jasm 对照 / 拒绝桶 / 预算 |
| [kasumi-verify.md](kasumi-verify.md) | Kasumi 专链端到端复现记录：五项门逐项数字、与参考实现口径差异点、通用链取证 |

## 行为文档

Grunt 家族逐混淆行为的描述文档（形态 + 配置面 + jasm 样例 + residue 现状）分层：行为 × residue 状态总表与遗留缺口清单（P1/P2/P3）已迁 RST（`obf-behaviors.rst`，原 `obf-behaviors/README.md` 已删）；语料罐台账在 `corpus-ledger.rst`（原 `corpus-ledger.md` 已删）。

- [obf-behaviors/grunt-v2-behaviors.rst](obf-behaviors/grunt-v2-behaviors.rst)——v2（Gruntpocalypse 2.x）35 个 transformer 行为文档。
- [obf-behaviors/grunt-v3-behaviors.rst](obf-behaviors/grunt-v3-behaviors.rst)——v3（Grunteon）39 个 transformer + GlslObfuscator 插件 + NativePipeline 阶段行为文档。

## 约定

代码组织约定见仓根 [CONVENTION.md](../CONVENTION.md)。
