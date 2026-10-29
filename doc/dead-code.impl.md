# dead_code 实现笔记

## 数据流

`Site{uri, methodIndex, victimCount}` → `DirtySet{uris, methods, insns,
attempted}`。SCAN 独立成段：不依赖其他残留的脏标记，管线里放最后，把全
部上游改写产生的死块一次收走。

## 位置索引

可达性 BFS 需要 `InsnId → order 下标` 的映射，用
`mjbase::asm_cfg::InsnIdHash`（mjbase 公开头，本仓不自带哈希）。

## 异常表感知的可达性

`reachability()` 是唯一判定入口，SCAN 与 REWRITE 共用：

1. 入口 = `order[0]`，按 op 展开后继（`follow()` 把 LabelId 解析回 order
   下标，`isEnd()` 无后继）；
2. 对每条异常表项，若 `[start, end)` 与可达集相交且 handler 未标记，则把
   handler 标为根继续 sweep；
3. 重复 2 直到没有新标记（handler 可能落在另一条保护区内）。

`labelPos(label, allowEnd)` 的 `allowEnd` 区分两种语义：`end` 锚允许等于
`order.size()`（区间到方法末），`start`/`handler` 不允许。

## 异常表收敛（擦除之前）

`rewriteMethod()` 先扫一遍 `cu->exc`：

- 区间内无存活指令 → 丢表项；
- `start` 不在存活指令上 → `createLabel(order[firstLive])`；
- `end` 不在存活指令上 → 吸附到其后第一条存活指令，找不到就
  `createLabel()`（方法末哨兵）。

任一端点解析失败（`sBgn == kNo || sEnd == kNo || sEnd <= sBgn`）整方法放弃
（return 0），不做半截改写。`excChanged` 但 `victims` 为空时也写回
`cu->exc`——异常表本身就是改动。

## JSR/RET

`JSR` 的后继 = 目标 + 顺序下一条（返回地址压栈）；`RET` 无静态后继（目标
在局部变量里）——按终止符处理，保守不越。Strict 帧重算对 jsr/ret 抛异常，
`mutateMethodWithFrames` 收敛为 false（整方法放弃）。

## 与 HotSpot 线性 walk 的关系

HotSpot 校验器对死代码并非「跳过」：线性 walk 走遍全部帧，且每个分支目标
/ 异常 handler 都必须有帧。所以死区必须在导出前物理消失，而不是留 NOP
或留帧——本残留擦掉死区，帧由 `rewireFramesAfterLower` 的 Strict 求解从
代码真值整体重算，不存在「锚在被擦指令上的孤儿帧」这类问题（手剪帧的
`smt_tools::pruneOrphanFrames` 已随 smt_toolbox.hpp 一起删除）。
