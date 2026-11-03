# doc 索引

口径分工：

- `doc/<residue>.md` 只写**混淆器会产出什么结构**——换个输入 jar 仍成立才写。
- `doc/<residue>.impl.md` 只写**本仓怎么还原**——固定三章：Scan / Rewrite / Proof。
- 具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。

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

## 约定

代码组织约定见仓根 [CONVENTION.md](../CONVENTION.md)。
