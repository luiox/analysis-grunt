# predicate_fold：常量操作数分支/switch 裁决

## 形态

grunt 往直线代码里插「常量 vs 常量」的假分支，真分支是原逻辑，假分支是
诱饵死块（调 JDK 方法再 throw 凑栈形状）：

```java
if (138795133 <= 21992595) {
    TT_MaxProfile.nmaxSizeOfInstructions((long) -1275583517581489779L);
    throw null;
}
```

switch 的键为常量时同理：目标臂唯一确定。

## 判据（S1）

1. 分支是 `ifeq..if_le`（1 操作数）、`if_icmp*`（2 操作数）或
   `tableswitch/lookupswitch`（1 键）；
2. 操作数由**紧邻的纯常量装载**产生、槽数严格吻合（`const_args` 工具箱，
   NOP 落点跳过——squeeze 先行后被压住的操作数区重新可见）；
3. 操作数全部 I32（引用/长整型不在本残留范围）。

混淆器必须让假分支恒走真分支，否则自己的 payload 到不了——裁决是静态事
实，无需求值器。

## 改写

taken → 区域首条操作数原地改 `goto_w`（保 srcPc）直奔目标；not-taken →
首条置 NOP；其余操作数与分支本体：非 indy 类擦除、indy 类 NOP 化（帧锚
保真）。**区域中段或分支本体带标签即整站放弃**——那是合并点，外部到达路
径的栈形状与区域产出耦合；区域首条的标签安全（到达起点即拿到折叠结果）。

## 证明

`classesIntact` + 改写前 `roundTripSafe` 门（原样往返不了的方法整方法跳
过，`skippedByRoundTrip` 计数）。
