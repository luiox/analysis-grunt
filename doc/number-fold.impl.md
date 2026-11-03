# number_fold 实现笔记

## 数据流

`Site{uri, methodIndex, anchorSrcPc, anchorOp, value}`（value =
`const_args::ValueLoad`，I32/I64/F32/F64 位形）→ `DirtySet{uris, sites,
attempted, failed, skippedByRoundTrip}`。

## Scan

- 逐方法 promote，按布局序起锚；锚点必须是常量装载且类型互斥分派三式：
  I32 → int 跨度 1（`<i32> I2L <i32> I2L LXOR L2I`），I64 → int 跨度 2
  （`<i64> L2I <i32> IXOR`），String → long 跨度（基 32 键 +
  `Long.parseUnsignedLong` + LXOR）。
- 跨度逐条钉死；NOP 夹层跳过（squeeze 遗留落点不驻栈，与
  `const_args::collectArgs` 同口径）。
- 包装扩展：跨度尾紧邻 `Float.intBitsToFloat(I)F` / `Double.longBitsToDouble(J)D`
  且实参类型与跨度产出吻合（I32→F32 / I64→F64）时，跨度延伸一条，产出从
  整数值升成位形浮点。
- 基 32 键严格解析：只认无符号数字前缀串（0-9a-v/A-V），超 64 位拒绝——
  生成侧 `Long.toString(v, 32)` 不产出符号/空白，多收即错。
- radix 恒 32（家族生成面）；`Long.parseLong` 等近邻不是生成面，引用三元
  组精确判。
- 标签门：跨度内除锚点外任何指令带标签 = 合并点（外部到达路径的栈形状与
  跨度产出耦合），整站放弃。
- 命中后锚点跳过整条跨度——三式锚点类型互斥，跨度内部无嵌套站点。

## Rewrite

- 按 `srcPc+op` 重定位锚点后**全量复核**：重新钉形状、重新求值、值与站点
  一致才动——同方法前站改写后不盲改。
- 锚点原地改结果常量装载（`rewrite_tools::makeConstLoad`：短形态优先，宽
  值落池），保 `srcPc`（下游 pcmap/调试行落点不变）。
- 跨度其余非 NOP 指令 `eraseBatch`（kRejectIfUsed：受害者被异常表引用即
  拒收——回滚锚点整站放弃，不留半改写态）。NOP 遗留落点不动，DeadCode 收尾。
- 帧链走 `mutateMethodWithFrames` 正统写回（prepare → lower → Strict 帧
  再生）：跨度可以出现在任何布局位置，被擦指令可能锚着调试属性/帧，用整
  链重算不逐帧修补。前置门 `roundTripSafe`（原样往返不了的方法不进改写面）。
- 改写不在本轮展开嵌套跨度（rewrite 只消费 ScanResult）：`a ^ b` 之后紧跟
  `i2l … lxor l2i` 的双层藏形，下一轮整链重复时折叠——README 的收敛口径。

## Proof

`classesIntact`：类数不增 + 脏类可重新解析。与全部残留同门。

## 已知边界

- `parseUnsignedLong` 的 `+`/`-` 号前缀语义（Java 8+）不收：生成面不产出，
  收了反而给判据开口子。
- NaN 位形不特判：`intBitsToFloat` 对 NaN 载荷的规范化 JVM 有自由度，LDC
  位形与运行期换算在载荷位上可差——控制流不受影响，不设门。
- 嵌套跨度（折叠产物再成跨度）靠整链重复跑到站点归零，不在单轮内迭代。
- fconst/dconst 起头的跨度不存在（生成面只从整型/字符串键起锚）——锚点类
  型门直接排除。
