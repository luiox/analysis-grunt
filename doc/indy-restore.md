# indy_restore：Grunt v2/v3 indy 调用点直接解密回填

## 形态

Grunt v2（`InvokeDynamicTransformer`，grunt2@36c0eef）与 v3
（`ReferenceObfuscate`，grunt3@e41e852）把**非 INVOKESPECIAL** 的方法调用
换成 `invokedynamic`，目标信息加密进 bootstrap 静态参数（两代逐字同源，
只有 heavy BSM access 位 `PUBLIC`→`PRIVATE` 一处历史差异，不进判据）：

```java
// simple 形（BSM 7 参：Lookup,String,MethodType,String,String,String,Integer）
invokedynamic <name> <indyDesc> { bsm } {
    "<encOwner>",      // 点分名（java.lang.Math），XOR 密文
    "<encName>",       // 方法名密文
    "<encDesc>",       // 方法描述符密文
    0 | 1              // isVirtual（生成面只发 0/1）
}
//  ↓ bootstrap
//  三段串各自经同类 (String;)String 解密器逐码元 `^ key`；virtual 时
//  indy desc 首参恒补 Ljava/lang/Object;（desc.replace("(", "(Ljava/lang/Object;")
//  ——receiver 槽就是原 virtual 的 receiver，改写是纯原地）

// heavy 形（BSM 7 参：Lookup,String,MethodType,String,Integer,Integer,Integer）
invokedynamic <name> <indyDesc> { bsm } {
    "<encOwner>",      // 经 BSM 体 LDC generalKey 解一层
    <magic1>, <magic2>,
    0 | 1
}
//  ↓ bootstrap：Class.forName(decOwner) → getAnnotation(Metadata) →
//  mv 版本门（BIPUSH 100; IF_ICMPGE）→ 遍历 d1，第 i 项经 magic2 解出
//  data1 整数串，`data1 % 59557 == magic2` 命中即取 d2[i] 经 magic1 解出
//  "name<>desc"
```

`encrypt` 是逐 UTF-16 码元自逆 XOR（`(code ^ xor).toChar()`），纯算术，
C++ 转写直解——无 jsim 面（`IndyFold` 的模拟兜底不消费本面站点）。

heavy 的 `@Metadata` 注解（v2 `@GruntMetadata` / v3 `@Metadata`，注解类名
可配置、名字不进判据）由生成器现场造：成员 `mv`(int，默认 100) /
`d1`/`d2`(String[])；`data1 = 59557 * q + magic2`（q < 1186）。

## 判据分级（README 残留一览表口径）

- **S2 结构事实**：BSM desc 指纹（simple/heavy 7 参，v2/v3 逐字同源）∧
  解密器窗口形状（`charAt; LDC K; IXOR; I2C` 单层，或 v3 材料通道双层
  `LDC K; INVOKESTATIC <self> ()I; IXOR; IXOR; I2C`，或 reobf 折叠键
  `LDC A; LDC B; IXOR; IXOR; I2C`，key = A^B）。
- **S1 语义后果**：解密产物与调用点吻合——simple 三段串解出合法
  owner/name/desc 且与 indy desc 互证（static 参数段全等；virtual 首参
  严格 `Ljava/lang/Object;` + 其余逐位一致 + ret 一致——按生成面钉死，比
  Kasumi 面的任意引用 receiver 更严）；heavy 的 `data1 % 59557 == magic2`
  完整性自证 + `name<>desc` 严格两段拆分 + 同一道 indy desc 互证。
  `isVirtual` 只认 0/1，越界即拒。
- **S3 版本敏感面**：heavy 的 `@Metadata` 数组解释（mv 门 + 59557 模数）
  挂 Era 门——`probeEra` 判 `Era::Grunt3` 才激活（Grunt3 双指纹：Grunt BSM
  desc ∧ 类级 `@Metadata` 注解或 `grunteon` 命名空间，见
  `doc/obf-behaviors.rst` 缺口 #15 v3 半边；
  v2 专面判据仍缺、如实挂账）。**simple 面不挂 Era**：判据全部结构自证
  （key 静态在场 + 产物与 indy desc 互证），误判需要「随机密文恰好解出与
  indy desc 吻合的合法方法描述符」，概率上排除——两代 simple 形逐字同源，
  挂 Era 只会白白拒绝 v2 现场而不会增加任何正确性。

## BSM→解密器键链（三路，按现场可达性）

BSM 名字不进判据（随机名），key 的定位按生成面的名字族事实走三路裁决：

1. **同名直连**（v3 material 配对类）：`plainDecryptName == plainBsmName`
   （源锚 grunt3 `ReferenceObfuscate.kt` L219-220，两者同为
   `bsmName1.substring(1, len-3)`）——simple BSM 名与一个在场
   `(String;)String` 解密器同名 → 直取其 LDC key。
2. **BSM 体跟随**（v2/v3 常态，BSM 体在场）：BSM 体内首条
   `INVOKESTATIC <self> (String;)String` 即解密器（源锚 L708）。
3. **名字族推导**（v3，BSM 被 NativePipeline 摘成 ACC_NATIVE 空壳时——
   boar 全量现场）：同代生成面把解密器名派生自 BSM 名
   （`decryptName = bsmName1.substring(2, len-1)`，L216；Kotlin substring
   右开区间 → 长度 len-3）：
   - c1：候选解密器 D 满足 `len(D) == len(BSM名) - 3 ∧ BSM名[2, len-1) == D`
     ——plain 形 → key；material 形 → 材料通道（fail-closed）；
   - c2（plain 解密器壳死、material 解密器在场）：BSM 名 N 与一个壳死
     `(String;)String` 同名（家族证据），material 解密器 M 满足
     `len(M) == len(N)+1 ∧ M[0, len(N)-1) == N[1,)`（都是
     `bsmName1[2, len(B)-3)`）→ M 的 LDC 即原始 decryptKey（源锚
     `createDecryptMethod` 两分支同一 `decryptKey` 常量，L224/L237），
     plain 站点用它直解。

heavy 的 generalKey 在 heavy BSM 体 `LDC K; INVOKESTATIC (String;I)String`
窗口里（源锚 `createHeavyBootstrap` L878-880）；BSM 壳死则 heavy 面无键
（fail-closed，boar 实证 heavy 站点为 0）。

## v3 RuntimeMaterial 联动（材料通道边界）

v3 `createDecryptMethod` 在类带 RuntimeMaterial 草稿时生成材料通道解密器
（`LDC key; INVOKESTATIC <self> ()I; IXOR; IXOR`），有效 key =
`decryptKey ^ materialKey()`，而 materialKey 读 canonicalKey/poison 静态
字段——boar 实证其运行期值由解密后的字符串池经 parseLong 转出（clinit
内 CFF 摊平），纯静态不可解（须模拟执行 + 上游供给链）。本 pass 材料通道
一律 fail-closed（`materialLane` 桶，只在解密器体形状能自证时记账），站点
逐位不动交后续批次。

## 改写（纯原地，槽位面）

v2/v3 只重定向方法调用，分派只有三态：static → `INVOKESTATIC`；virtual →
`INVOKEVIRTUAL` / `INVOKEINTERFACE`（receiver 槽不插不删——indy desc 的
Object 首参就是原 receiver 槽）。原地换指令保 `srcPc`，帧链写回走
`mutateMethodWithFrames` 正统帧链（与 indy_decode 同源）。接口落点靠旁证：
容器普查优先（权威）；CLI `--jdk` 注入的 JDK 接口集是穷举普查，**成员与否
双向作证**（在集 = 接口；不在集 = 类）；两处皆无 = fail-closed 拒绝
（invokevirtual 误调接口方法运行时 `IncompatibleClassChangeError`）。

## 台账

`ScanResult` 拒绝桶：`keyMissing`（键链三路全断，含壳死无亲）/ 
`malformedArguments` / `decodeFailed`（解密失败、产物空、kind 位越界）/
`descMismatch`（S1 互证不符）/ `magicMismatch`（heavy：d1 全扫无自证命中）/
`metadataMissing`（owner 类无 mv/d1/d2 形注解或 Era 未判）/ `materialLane` /
`interfaceUnattested` / `classesSkipped`。`DirtySet` 按 static/virtual 分列。
proof residual = 改写后容器里仍带 Grunt simple/heavy BSM desc 指纹的 indy
调用点数（材料通道与键缺失站点按边界如实留在 residual 里；非 Grunt indy
不进口径）。

## 语料账（boar-main.jar，Grunteon 全链产物，3065 类）

- 单跑 `grunt.IndyRestore`：`era=Grunt3`；指纹命中 78154 站点，还原 30924
  （static 29847 + virtual 1077），failed=0、roundtrip-skip=0，proof ok；
  residual=47230（materialLane 42228 + keyMissing 5002，边界如实挂账——
  前者为 RuntimeMaterial 运行期材料字段，后者为解密器被 NativePipeline
  连壳摘除的类）。
- 组合链 `grunt.IndyRestore,grunt.StringPoolFold`：IndyRestore 先行把 clinit
  里被 indy 蹦床藏面的 arrayInit 调用还原成直线静态蹦床，StringPoolFold
  从上一批的 0 折叠接通到 19342 站点 / 19244 折叠（arrayed 形，failed=98，
  proof ok，residual=0）。
- JVM `-Xverify:all` 门：单跑与组合链产物的逐类失败集与输入**逐类全等**
  （227 基线同挂，无新增失败）；类数不增（3065 → 3065）。
- `corpus-regress` 双罐（boar-main + boar-main-origin）：2 罐 0 失败；
  origin 罐（未混淆）两 pass 全零静默通过（era=Unknown fail-closed 正确）。

判据源锚与实现细节见 [indy-restore.impl.md](indy-restore.impl.md)。
