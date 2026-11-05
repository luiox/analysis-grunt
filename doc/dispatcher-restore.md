# dispatcher_restore：tableswitch 分发器调用点反查

## 形态

Grunt v3（`InvokeDispatcher`，grunt3@e41e852）把同返回类型的多个方法调用
点合并进一个分发器：按返回类型分组（每组 ≥2 个调用点、`MaxHandles=10`、
`MaxParams=10`、参数含 primitive 的调用点跳过），每类生成
`PRIVATE + STATIC` 分发器方法、名 `dispatcher_<10 随机字符>`、desc
`(N × Ljava/lang/Object; I) ret`（N = 组内最大参数槽位数，key 就在槽 N）。

分发器体（生成面逐字）：

```java
// grunt3 InvokeDispatcher.kt buildStageImpl
Lhead: iload N                    // key 参数槽 = Object 参数个数
tableswitch { min: startCase, max: endCase,
              cases: { Lc0 .. Lck },
              default: Lhead }    // 判据锚点：default 回跳方法头
Lci:
  { aload k; checkcast <实参型> }× // case 转发：载实参 + CHECKCAST
  invokestatic|invokevirtual <真目标>
  <ret 型 return>                  // 键 → 目标映射编译期可重建
```

调用点（生成器**就地改写原调用指令的 owner/name/desc**，opcode 不变——
静态源调用点保持 INVOKESTATIC，是唯一可验证形）：

```java
<原调用的实参栈形不动>
{ aconst_null }×addNulls          // 补齐 maxLoadOp 缺参（addNulls = N - 实参槽数）
<int key>                         // access key 常量（bipush/sipush/ldc 任意装载）
invokestatic owner.<分发器> (N × Object; I) ret
```

case 转发体的实参装载有两个生成面细节：virtual 目标的 case 体从槽 0 起连
载（receiver 由 CHECKCAST 到 owner 型表达，没有显式 receiver load——源
码 `stack` 下标编排的产物）；实参全部引用型（primitive 参调用点在分组时
被跳过，`it.getLoadType()` 对 L/[ 型一律 ALOAD）。

## 为什么可以静态还原

回填一个调用点需要回答「这个 key 的真目标是谁」。生成器把答案全留在类文
件里：键区间在 tableswitch 的静态编码（min/max/cases 一一对应），键 → 真
目标映射在每个 case 转发体的唯一 invoke 里——**S2 级判据天然在场，旁证
不需要**。jsim 也不需要：key 是调用点紧邻常量、键表是静态编码，纯常量表
直查。

判据走结构自证：**头锚**（首条 ILOAD 恰为 key 槽 N + 第二条 TABLESWITCH
+ default 回跳方法头 + 无异常表）钉住分发器，逐 case 直线转发体（载实参
+ CHECKCAST + 唯一 invoke + ret 型 return）重建键表；调用点侧 key 常量紧
邻压栈 + null 跑长度与目标栈形互证。任何一环缺席或被后续混淆打碎，
fail-closed 拒绝——residual 如实计，不猜。

**default 回跳方法头是识别锚点**：普通 switch 不会把 default 指回方法第
一条指令；生成器这么做是防 key 越界死循环的习惯形。这一锚把 CFF 摊平后
的 dispatcher 岛（default 落分发块）与同名形完整排除。

## 关键事实（判据的地基）

- **调用点 opcode 不变是互证的一部分**：生成器只改 owner/name/desc。静态
  源调用点 = INVOKESTATIC；实例方法的调用点经 ReferenceObfuscate 之外的
  路径出现 INVOKEVIRTUAL 分发器调用的形不存在（分发器必 static）——
  INVOKESTATIC 是唯一可验证形。
- **null 跑长于补位数是合法形**：原实参尾部的 null 字面量与生成面补位
  `ACONST_NULL` 同形不可分辨，null 跑 ∈ [addNulls, N]（下界 = 无 null 实
  参，上界 = 验证器强制的满栈形）——回填只擦**末段** addNulls 个，前段
  null 原样保留。
- **分发器本体与键表不在本 pass 删除**：调用面归零后由 DeadCode /
  DeadMember / 压池统一回收（与 string_pool_fold 同口径——改写面只动调
  用点，供给链留给专项批次）。
- **无 Era 门**：v2（grunt2@36c0eef）redirect/ 下只有
  FieldScramble/InvokeDynamic/MethodExtractor/MethodScramble/
  StringEqualsRedirect——InvokeDispatcher 是 v3-only 形态，判据内无版本敏
  感面；形态锚自证家族。
- **键表是连续区间**：`startCase..endCase` 连续编码，`min/max/cases` 缺格
  即形状不符（JVM tableswitch 本就要求连续）；lookupswitch 形不在生成面。

## 与相邻形态的区别

- **CFF 的 dispatcher 岛（缺口 #4）**：那边是 state → 块序的控制流摊平，
  default 落分发块、case 体是块跳不是直线转发；本残留 default 回方法头、
  case 体直线转发 + ret 收尾。CFF 在 InvokeDispatcher **之后**跑（config
  顺序）会把分发器体摊碎——头锚缺席，本 pass fail-closed 留给缺口 #4。
- **ReferenceObfuscate（缺口 #2）**：那边把调用点 indy 化——真 jar 上
  InvokeDispatcher 在 ReferenceObfuscate **之前**跑，调用点多已变 indy，
  要先经 IndyRestore 还原出 INVOKESTATIC 形本 pass 才能接上。
- **proxy_inline / field_access_proxy（S2 调用点改指）**：改写原语同源
  （调用指令原地换 + srcPc 保留），但那边的映射从代理体单向读出，本残留
  的映射要从键表 + 调用点 key 双侧互证。
- **string_pool_fold（供给链存活性口径）**：同款「本体不删，交下游死码/
  死成员回收」的口径；那边供给链被 clinit 前导引用而存活，本残留分发器
  被键表 case 体自引用（default 回跳）+ 无调用面后成为零引用私有静态方
  法，DeadMember 可收。
