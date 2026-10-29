# dead_code：不可达指令码清理

## 形态

predicate_fold 裁决掉假分支后，诱饵块从此再无控制流入口；return/athrow
之后的残段、永不命中的 switch 臂同理。这些指令还在字节布局里，反编译出
来就是「死块」。

## 判据（S2）

从入口做控制流可达性 BFS：顺序后继 + 分支/switch 全目标；return/athrow/
ret 无后继。可达集合之外的指令即不可达（fall-through 已计入，不存在歧
义）。

**异常表是隐藏控制流边**：保护区间内任一可达指令都可能抛出并进入
handler，所以 handler 的可达性不能只看有没有显式跳转。做法是以「区间与
可达集相交」为根把 handler 也纳入 BFS，并迭代到不动点——handler 自己
落在别的保护区里时继续向外传导。带异常表的方法不再整体跳过（grunt 方法
几乎全带 try/catch，跳过等于永不清死区）。

## 改写

先收敛异常表，再擦指令：

- 区间全死 → 删表项（这段代码抛不出异常，handler 也进不来）；
- `start` 锚在死指令上 → 向前吸附到区间内第一条存活指令；
- `end` 锚在死指令上 → 向前吸附到其后第一条存活指令，尾部全死则落方法
  末哨兵（`createLabel()` 无参形态）。

吸附必须发生在擦除之前：擦除后原锚点已不存在，`createLabel` 无从解析。
吸附规则与 `frame_rewire` 对 LNT/LVT 的悬垂处理一致——区间吞并被删段，
不产生悬空端点。

随后 `eraseBatch` 整批擦除，布局缩短交 lower 的 pcmap。

## 帧

本残留不剪帧。改写走 `rewrite_tools::mutateMethodWithFrames`（promote 一
次 → mutate → `prepareNestedAttrsForLayout` → `lowerMethod` →
`rewireFramesAfterLower`），帧由 Strict 求解从改写后的代码真值整体重算。

## 链序约束

**`grunt.PredicateFold` 之后必须跟 `grunt.DeadCode`**。PredicateFold 折叠
假分支后留下的死区里可能还有 switch，其分支目标没有帧（Strict 只为可达
代码生成帧），链尾导出时 HotSpot 会报「Expecting a stackmap frame at
branch target」。死区由本残留清掉，中间态才合法。

## 证明

`classesIntact` + 改写前 `roundTripSafe` 门。
