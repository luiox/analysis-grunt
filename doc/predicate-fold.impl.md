# predicate_fold 实现笔记

## 数据流

`Site{uri, methodIndex, branchSrcPc, branchOp}` → `DirtySet{uris, sites,
attempted, failed, skippedByRoundTrip}`。

## 裁决表

| op | taken 条件 |
|---|---|
| ifeq/ifne/iflt/ifge/ifgt/ifle | v 与 0 比较 |
| if_icmpeq..if_icmple | v1（深）与 v2（顶）比较 |
| tableswitch | lo ≤ key < lo+span 取臂，否则 default |
| lookupswitch | 命中 keys[i] 取臂，否则 default |

## 复核纪律

rewrite 不信 scan 的区域收集结果：按 `srcPc+op` 重定位分支后**重新收操作
数区、重新裁决**——同方法前面的站点改写后（首条变 ICONST_0 / NOP）后续站
点的紧邻关系可能已变，盲改就是半截改写。

## 写回与帧

本 pass 的写回是类级批驱动：逐方法 promote → DOM 改写 → 全部方法 lower 成
功才 `writeClass`（任一失败整类放弃，session 丢弃，半改写态不外泄）。**不
走 `mutateMethodWithFrames` 的 Strict 帧再生**：改写不新增合并点、不新增
帧位（TAKEN 的 `IFEQ` 目标是原分支目标，本来就有帧；延续段经条件跳转的
fallthrough 保持可达），旧 SMT 由 lower 的 pcmap 重定位即真值——被擦区域
对栈的净效应为零（操作数全部被分支消费），后继帧内容无需变更。

这条契约的前提是形状纪律：**改写不得引入无条件跳转、不得新增合并点**。
HotSpot 线性走查要求无条件跳转之后的段首带帧，而本 pass 不再生 SMT——
一旦破例插 `goto`，被擦延续段的段首无帧，导出即
`Expecting a stack map frame`。NOT-TAKEN 折叠留下的分支目标死块带原帧，
中间态合法，由下游 `grunt.DeadCode` 清理。

## 已知边界

- `ifnull/ifnonnull/if_acmp*`：引用判空不可静态确定，不在范围。
- 操作数区中段有标签 = 合并点，整站放弃。
