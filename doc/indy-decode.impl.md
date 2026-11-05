# indy_decode 实现笔记

## 数据流

`ScanInput{era, jdkInterfaces}` → `ScanResult{sites, rejects, 六桶计数}` →
`DirtySet{uris, 7 形态分列计数, failed, skippedByRoundTrip}` →
`ProofResult{ok, failed, residual}`。**注入面**：文件系统副作用（JDK 接口
普查）收敛在 scan 之前——CLI `--jdk` 传入 JDK 根，adapter 在 `probeEra`
判为 Kasumi 时才扫描注入；残留本体零文件系统遍历，`jdkInterfaces` 为空 =
无任何旁证（fail-closed 语义，见 Scan 章）。

## Scan

- **两轮**：第一轮全容器普查（各类 access 位收接口集、逐方法收解密器 key
  与 BSM→解密器映射），第二轮逐方法逐 `INVOKEDYNAMIC` 判定。key 按**内部
  类名**归账。
- **S2 双钉选面**：BSM 8 参描述符指纹（`scan_tools::bsmEntries` 读
  BootstrapMethods）+ 解密器体形（`i2c` 回溯两层 `ixor` 取常量异或，兼容
  单层）。指纹不钉的 indy 静默跳过——非本家调用点不记账。
- **Era 门**：`probeEra ≠ Kasumi` 整站拒绝（`eraBlocked` 桶）。mode/extra
  表是 S3 换代面，Era 分支是它唯一合法的落点。
- **key 链**：`bsmDecrypt[bsm] → dec → decryptors[dec] = key`，任一环断 =
  `keyMissing`（BSM 体里第一条直调本类 `(String;)String` 的 invokestatic
  即映射，解密器体形不成立则 key 缺席）。
- **bsmArgs**：恰 5 个 `[Integer,String,String,String,Integer]`，不符 =
  `malformedArguments`。
- **解密**：自写 MUTF-8 码元解码（lone surrogate 是合法形态，库的严格
  UTF-8 解码会拒真），逐码元 XOR key 低 16 位后按 Java 规则重编码；owner
  点分转内部名；ctor 的 encDesc 解出空 → init desc 由 indy 描述符参数段
  合成（描述符里 `)` 只在参数段尾出现一次，取最后一个 `)`）。
- **S1 互证定案**：static 参数与返回全等；virtual indy 参数 = 目标参数 +
  receiver 槽（首参引用型）；字段 4 路 `()T / (T)V / (Object)T /
  (Object,T)V`；ctor 返回 `Lowner;` 且参数段一致。不符 = `descMismatch`。
- **virtual 落点分派**：容器普查优先（权威）；JDK 接口集是穷举普查，
  **成员与否双向作证**——在集 = 接口（invokeinterface），不在集 = 类
  （invokevirtual，ArrayList 这类 JDK 类目标靠缺席证据落对）；**空集 = 无
  任何旁证**，目标又不在容器时整站 fail-closed 拒绝（`interfaceUnattested`
  ——invokevirtual 误调接口方法运行时 `IncompatibleClassChangeError`）。

## Rewrite

- 按 uri 聚合；`roundTripSafe` 前置门（未改会话先往返），跑不完整方法
  整方法跳过。
- **纯原地三形态**（static / getstatic / putstatic）：换 op + `setMember`
  换 CP 引用（先 intern 后动 DOM），`srcPc` 保留，锚在 indy 上的调试行 /
  标签照常重定位。换不动则 op 还原——失败不留半截指令。
- **插入式四形态**（virtual / getfield / putfield / ctor）：锚保留设计 =
  **indy 本体原地换指令**，spill/reload（或 `NEW;DUP`）序列插在 indy
  之前。全局不变量「插入把 srcPc 清成 kNoPc」在这里的解法：本残留只插不
  删，全方法没有失去落点的旧 pc，`lowerMethod` 的 pcmap 重定位不会
  `RelocationBadTarget`；锚在 indy 旧 pc 上的 LNT 行/标签随 indy 迁到新
  布局；插入块内部无标签无分支，不产生新合并点。
- **槽位面**：spill 槽从 `cu->maxLocals` 起排（receiver/value + 实参按
  `slotWidth` 累计，cat2 双槽），成功后抬 `maxLocals` 到新水位；超 u16
  整站放弃。插入失败 `eraseBatch` 回滚再报 failed。
- **帧链**：`mutateMethodWithFrames` 整体重算（prepareNestedAttrsForLayout
  → lowerMethod → rewireFramesAfterLower）；失败粒度 = 类，整类放弃回写，
  7 形态分列计数只在类写回成功后归账。

## Proof

`pass_clock::Suspend`（校验期挂 proof 时钟）+ 类数不增 + 脏类可重新解析
（与全部残留同门）+ **residual**：改写后容器里仍带 8 参指纹的 indy 调用
点数，语料单罐门要求 0。

## 语料账本（单罐验收）

- **来源**：morpher 仓 `mjt-deobf/test/kasumi/SnakeGame-obf.jar`
  （origin/main @ 077b0ac64，树内未跟踪样本）；582,974 B；sha256
  `c3481fb16f66ea9fade86e601a73b651db984666de18fe356bb439cc874f77fc`。jar
  不进仓，副本放 run-lab 语料位。
- **基线**：morpher verify.md（树版实现）——627 indy 全还原，residual=0
  （static 478 / virtual 72 / getfield 4 / putfield 4 / getstatic 28 /
  putstatic 34 / new 7）。
- **本仓实测**（grunt_cli，`--jdk $JAVA_HOME`）：scan `era=Kasumi
  jdk-ifaces=2697 total=627 sites=627 rejects=0`（六桶全 0）；rewrite
  `classes=7 sites=627 failed=0 roundtrip-skip=0`（形态计数与基线逐项
  同）；proof `ok residual=0`——与基线逐位对齐。
- **Era 实证**（同罐）：`dev/kasumi/obfuscator` 命名空间 25 类 + 8 参 BSM
  描述符全命中 + `@Metadata` 注解每类一个——三指纹任一即判。

## 已知边界

- 解密产物空 owner / 非 ctor 空 name → `decodeFailed` 拒绝，不猜。
- 解密器形状变体（`i2c` 前多一层算术等）不认——换代面，Era 门兜底整站
  拒绝。
- `--jdk` 不传且容器内无落点旁证的 virtual 站点整站拒绝：fail-closed 优先
  于覆盖率。
- `ConstantCallSite` 假设：可变调用点形态不在面内。
