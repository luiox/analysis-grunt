# flags_strip.impl

判据等级：S2 结构事实（编译器不可能生成的标志组合）。

## Scan

纯读。逐类开只读会话，直接看 backing 的 `ClassFile`（不 promote，方法体与
本残留无关）。三个门全过才收站点：

1. `major >= 55`（Java 11）：nestmates 之前 javac 会生成 `access$000` 这类
   static synthetic 访问器，旧版本类文件的 static synthetic 可能是真的；
2. 类不是 enum（`ACC_ENUM` == 0）：`$VALUES` 等枚举机制字段是真实的
   static synthetic；
3. 成员是 static。

满足后：

- static 方法带 `ACC_BRIDGE` 或 `ACC_SYNTHETIC` → 站点，`clearMask` = 两标志
  按位与的结果；
- static 字段带 `ACC_SYNTHETIC` → 站点，`clearMask` = `ACC_SYNTHETIC`。

保守面（不收站点）：实例方法的 `ACC_BRIDGE`（可能是真实泛型桥接器）、实例
成员的 `ACC_SYNTHETIC`（`this$0` 等）、类级 `ACC_SYNTHETIC`。站点用 backing
索引定位，掩码在 scan 定死。

## Rewrite

纯写。按 uri 分组，逐类开 会话 → **按 scan 给的掩码清位**（不回看判据，
rewrite 只消费 ScanResult）→ `writeClass` → 回写容器。无 promote/lower：
不改方法体，没有重定位面。索引越界的站点计数（`sitesOutOfRange`）不猜。

## Proof

类数不增 + 脏类逐个可重新解析（与 constant_call 同一口径）。标志清理不改
字节码结构，无帧重算面，逐类加载校验由语料回归覆盖。
