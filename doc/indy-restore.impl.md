# indy_restore 实现笔记

## 数据流

`ScanInput{era, jdkInterfaces}` → `ScanResult{sites, rejects, totalIndy,
keyMissing, malformedArguments, decodeFailed, descMismatch, magicMismatch,
metadataMissing, materialLane, interfaceUnattested, classesSkipped}` →
`DirtySet{uris, sites, classes, failed, skippedByRoundTrip, rewrittenStatic,
rewrittenVirtual}` → `ProofResult{ok, failed, residual}`。Era 消费口径：simple
面判据自证不挂门；heavy 面只在 `Era::Grunt3` 激活（`probeEra` 判据本批扩了
Grunt3 双指纹，见 knowledge/era.hpp）。JDK 接口集由 adapter 在 Era 非
Unknown 时经 `--jdk` 注入（`scanJdkInterfaces` 与 indy_decode 单源），残留
本体零文件系统副作用。

## 算法（源锚点）

**encrypt 自逆**（grunt3@e41e852 `ReferenceObfuscate.kt` / grunt2@36c0eef
`InvokeDynamicTransformer.kt` 逐字同源）：

```kotlin
fun encrypt(value: String, xor: Int): String =
    value.map { (it.code xor xor).toChar() }.joinToString("")
```

C++ 侧 `mutf8ToUnits` + `decryptXor`（每码元 `^ key` 低 16 位）：MUTF-8 直
解不过严格 UTF-8——密文里合法存在 Java 孤代理码元（classfile 以 CESU-8 形
3 字节承载），严格解码器会拒收。与 indy_decode 的同名面同形独立实现
（string-pool-fold.impl.md「互逆面各有独立实现」的既定口径）。

**名字族**（grunt3 L214-220；Kotlin substring 右开区间，基 B = bsmName1）：

```kotlin
val bsmName1      = referenceHelperNameBase(classNode, config)      // B
val bsmName2      = B.substring(1, B.length - 1)                    // heavy BSM 名
val decryptName   = B.substring(2, B.length - 1)                    // D：len = len-3
val materialKeyName = B.substring(1, B.length - 2)                  // K：len = len-3
val plainBsmName  = B.substring(1, B.length - 3)                    // P：len = len-4
val plainDecryptName = P                                            // 同名直连的根
```

三条键链的形状事实全部由此钉死：

- 路由 1（同名直连）：plainBsm（BSM）与 plainDecrypt（解密器）同名；
- 路由 2（体跟随）：BSM 体首条 `INVOKESTATIC <self> (String;)String`
  （L708 等 5 处）；
- 路由 3 c1：`D = B[2, len-1)`，`len(D) = len(B) - 3`；c2（plain 壳死）：
  `N = P, M = D` ⇒ `len(M) = len(N)+1 ∧ M[0, len(N)-1) = N[1,)`（都是
  `B[2, len-3)`）。

c2 的 key 正确性源锚：`createDecryptMethod(className, decryptName,
decryptKey, materialKeyName)`（L224）与 `createDecryptMethod(classNode.name,
plainDecryptName, decryptKey, null)`（L237）——**两分支同一 `decryptKey`
常量**，material 通道的 LDC 是异或前的原始 key，plain 站点可直接用。

**heavy**：`createHeavyBootstrap`（L845 起）体 `ALOAD(3); LDC(generalKey);
INVOKESTATIC(className, decryptName, "(Ljava/lang/String;I)...")`——generalKey
取 BSM 体 `INVOKESTATIC (String;I)String` 紧前一条 int 常量装载；owner 解密
后 `Class.forName` + `getAnnotation` + `mv()` 门（BIPUSH 100; IF_ICMPGE）+
d1/d2 全扫。`data1 = 59557 * q + magic2` 自证在 C++ 侧按
`parseIntStrict(data1) % 59557 == magic2` 复算（JVM IREM 与 C++ % 在同号域
语义一致；data1 恒非负）。

## Scan

- **collectClassKeys**（每类一遍，纯读）：
  - `(String;)String` 方法：promote 后 `scanDecryptKey` 提窗口——I2C 回溯
    两层（布局序 = DOM order，对齐 indy_decode 的 key 链提取）。单层
    `LDC K; IXOR` → Plain；`LDC K; INVOKESTATIC <self> ()I; IXOR; IXOR` →
    Material（记原始 K）；`LDC A; LDC B; IXOR; IXOR`（reobf 折叠键）→
    Plain(A^B)。无 Code / promote 失败 / 无窗口 → `deadStringMethods`
    （c2 家族证据）。
  - simple/heavy BSM 方法：体在场才进表——simple 取体首条直调本类
    `(String;)String`；heavy 取 `(String;I)String` 紧前的 int 常量。
- **resolveSimpleLane**（键链三路，按 BSM 方法名裁决）：同名 → 体跟随 →
  族推导。族推导只认 c1（len-3 且 B[2,) 前缀）与 c2（len+1 且 dead 同名
  证据），其余一律 Missing——**不认** `plainDecryptName`（len-4）的族关系：
  那条边由路由 1 的同名面覆盖，混进族推导会把 material BSM 的站点误接到
  plain 解密器上（解出垃圾死在 S1，虽 fail-closed 但记账口径被污染）。
- **readMetadata**（heavy owner 类）：类级可见注解扫描（`attrSafeToDecode`
  预检先行，mjbase #158 门）；形状判据 `mv/d1/d2` 元素名齐备 + `mv >= 100`
  + 注解类在容器内且带 `mv()I / d1()[Ljava/lang/String; / d2()[Ljava/lang/String;`
  抽象方法三件套（ACC_ANNOTATION 位不进 gate——方法表形状是 v2/v3 生成面
  逐字同源的部分）。d1/d2 长度相等非空才收。
- **judgeSite**：粗找 = INVOKEDYNAMIC 全量起锚；细找按 S2（BSM desc 指纹）
  → 键链 → S1（解密 + desc 互证）→ S3（heavy Era 门 + magic 自证）→
  接口旁证（容器普查优先 / JDK 集双向作证 / 两无 = fail-closed）。

## REWRITE

`mutateMethodWithFrames` 正统帧链写回（FrameContext 全容器层级图一次构建）。
站点按 uri 聚类；roundTripSafe 前置门；`rewriteSite` 按 `indySrcPc` 重定位 +
复核（必须还是 INVOKEDYNAMIC）后纯原地换 op + `setMember`——**不插指令**
（v2/v3 无 spill 面，virtual 的 receiver 槽就是 indy 首参）。换指令失败把 op
还原，不留半截。形态计数在类写回成功后归账（类级放弃不许虚报站点，对齐
indy_decode 口径）。

## PROOF

`classesIntact`（类数不增 + 脏类可重新解析，pass_clock::Suspend 挂起全量
盘点）+ `countGruntIndyResidual`：改写后容器里 BSM desc 仍全等
`kGruntSimpleBsmDesc`/`kGruntHeavyBsmDesc` 的条目上还挂着的 indy 调用点数。
residual 只计 Grunt 指纹站点——材料通道（`materialLane`）与键缺失
（`keyMissing`）站点如实留在 residual 里，语料单罐门按边界口径解读（见下）。

## 语料账本（boar-main.jar，3065 类）

单跑 `grunt.IndyRestore`（`--jdk` 注入 JDK17 接口集 2697 条）：

```
scan:    era=Grunt3 total=78154 sites=30924 rejects=47230
         (key=5002 malformed=0 decode=0 desc=0 magic=0 metadata=0
          material=42228 iface=0) skip-classes=0
rewrite: classes=717 sites=30924 failed=0 roundtrip-skip=0
         (static=29847 virtual=1077) proof=ok residual=47230
```

与普查口径的对账：30924 = plain 直连/体跟随 + c2 家族捐赠 + 非 material
类的 c1 站点；47230 = material 通道 42228（RuntimeMaterial 运行期材料字段，
静态不可解——边界挂账）+ keyMissing 5002（解密器被 NativePipeline 连壳摘
除）。材料通道要接通需要模拟执行 + 上游供给链（原语缺失，按任务边界停下
报，不扩基建）。

组合链 `grunt.IndyRestore,grunt.StringPoolFold`（缺口 #2 → 缺口 #1 供给链
接通）：

```
IndyRestore:    同上
StringPoolFold: pool-classes=717 triple-hits=19342 sites=19342
                rewrite: sites=19244 (arrayed=19244) failed=98
                proof=ok residual=0
```

上一批 StringPoolFold 单跑在 boar 上为 0 折叠（724 池类全被 indy 藏面拒
绝）；本链接通后 19244 折叠，98 失败 = material/keyMissing 站点类（其
arrayInit 调用仍在 indy 里，arrayinit 拒绝 7 类）。JVM `-Xverify:all` 门：
单跑与组合链产物的逐类失败集与输入逐类全等（227 基线同挂，无新增）；类数
3065 不增。`corpus-regress` 双罐 0 失败（origin 罐两 pass 零站点静默通过）。

## 边界与不做

- **材料通道不猜**：materialKey 读 canonicalKey/poison（运行期值由解密后
  字符串池经 parseLong 转出，clinit CFF 摊平）——静态不可解，须模拟执行
  + 上游供给链，原语缺失即停（任务边界「若遇到必须模拟执行的面，停下报」）。
- **v2 专面不建**：heavy 的 `@GruntMetadata` 注解名可配置、heavy 关闭时注
  解类不生成，不能当版本判据——`probeEra` 的 Grunt2 判据如实挂账
  （era.hpp 明文记载），v2 现场的 heavy 面按保守口径关闭（simple 面不挂
  Era，两代同源自证）。
- **BSM/解密器/注解清理不做**：改写只动调用点；指纹 BSM 条目、解密器、
  @Metadata 注解交后续清理面（DeadMember 引用普查 + metadata-strip 类面）。
