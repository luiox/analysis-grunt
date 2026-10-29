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

## 约定

代码组织约定见仓根 [CONVENTION.md](../CONVENTION.md)。
