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
数区、重新裁决**——同方法前面的站点改写后（首条变 goto_w / NOP）后续站
点的紧邻关系可能已变，盲改就是半截改写。

## 擦除与帧

擦除后帧锚在被擦指令上的帧成为孤儿。本残留不自己剪帧：改写走
`rewrite_tools::mutateMethodWithFrames`（promote 一次 → mutate →
`prepareNestedAttrsForLayout` → `lowerMethod` → `rewireFramesAfterLower`），
帧由 Strict 求解从改写后的代码真值整体重算，孤儿帧不存在（见
rewrite_toolbox.hpp 头注）。

**链序约束**：折叠留下的死区在链尾导出前没有帧（Strict 只为可达代码生成
帧），必须由紧随其后的 `grunt.DeadCode` 清掉，否则 HotSpot 报
「Expecting a stackmap frame at branch target」。

## 已知边界

- `ifnull/ifnonnull/if_acmp*`：引用判空不可静态确定，不在范围。
- 操作数区中段有标签 = 合并点，整站放弃。
