# string_pool_fold 实现笔记

## 数据流

`ScanInput{}`（无旁证注入面占位——明文全部容器自证）→
`ScanResult{sites, rejects, poolClasses, tripleHits, idxMissing,
arrayInitBlocked, decryptorBlocked, classesSkipped}` →
`DirtySet{uris, sites, classes, failed, skippedByRoundTrip, foldedLiteral,
foldedArrayed}` → `ProofResult{ok, failed, residual}`。Era 不消费（v2/v3
形态是配置不是版本键，缺口 #15 不在本批面内）。

## 算法（源锚点）

自逆 XOR 流（状态与密文无关 → decrypt == encrypt），实现
`gruntCharXorStream`（JVM int 语义在 C++ 无符号域显式完成：溢出回绕、移位
`& 0x1F`）：

```kotlin
// grunt2@36c0eef StringEncryptTransformer.kt L389-403
// == grunt3@e41e852 StringArrayedEncrypt.kt L405-419（两分支逐字同源）
var n = key; var l = seed; var n2 = classKey xor n
for (i in cArray.indices) {
    n2 = n2 xor l.toInt() xor i.inv()        // state ^= (int)seed ^ ~i
    n2 = n2 xor (n - i * cArray.size)        // state ^= key - i*len
    n2 = (-n2 * n) or i                      // state = (-state*key) | i
    cArray[i] = (cArray[i].code xor n2).toChar()
    val n3 = i and 0xFF                      // 状态随 i&0xFF 旋转
    n = (n shl n3) or (n ushr -n3)
    l = l xor n3.toLong()
}
```

供给链发射面：v2 `StringEncryptTransformer.kt` L95-124（arrayInit 块 +
clinit 前导），v3 `StringArrayedEncrypt.kt` L107-136 + L447-470（同构，
`config.carray` 分支选密文形态）；解密器编排 `createDecryptMethod`
（v2 L295-330 == v3 同构：`INT(classKey); ILOAD(3); IXOR; ISTORE(4);
ICONST_0; ISTORE(5)` 头 + `[CJI)` 槽位）。两分支 commit 锚点见
`knowledge/grunt_names.hpp` 头注。

## Scan

- **池链三件套钉形**（`buildPoolChain`）：
  1. **前导四连**：`<int poolLen>; ANEWARRAY java/lang/String;
     PUTSTATIC <self String[] static>; INVOKESTATIC <self ()V>`——clinit 内
     全量滑窗扫候选，多个不同解 fail-closed（`多个池分配前导`）；池字段必须在
     场且 static。
  2. **直线蹦床跟随**：前导引用的 `()V` 允许是直线静态蹦床（体恰
     `INVOKESTATIC <self> ()V; RETURN`，深度封 2、目标异于自身防环）——
     Grunteon 全链把该调用藏进 indy 机器（boar 语料实证），上游
     IndyDecode/IndyFold 还原后即此形；`[INVOKEDYNAMIC; RETURN]` 形态
     fail-closed 报独立原因（`arrayInit 调用被 indy 蹦床隐藏`），不评 BSM。
  3. **arrayInit 块序列**（`parseArrayInit`）：整段直线（无标签/无异常表/
     无分支），逐块 `GETSTATIC <池>; <int idx>; 密文; LONG seed; INT key;
     INVOKESTATIC <dec> ([CJI); AASTORE`，块数与前导池长互证。**形态判别按
     常量 KIND**：密文位是 String 常量 → literal（钉 toCharArray 形），
     int 常量 → arrayed（len 经 iconst/bipush/ldc 任意装载，NEWARRAY T_CHAR
     钉 `a==5`，码元组 `DUP; <int e>; <int code>; CASTORE` 下标乱序即拒）。
  4. **解密器三窗口**（`extractClassKey`）：52 条核窗口（源序 L4~L11 逐条，
     槽位 state/counter 自捕获、互证一致，`k==3 && l==1` 由 desc 静态唯一，
     旋转掩码 255 由常量值钉——汇编码归一后 `sipush 255` 可能成 `ldc 255`，
     钉 opcode 会失配）+ init 窗口（`<int c>; iload k; ixor; istore s;
     iconst_0; istore i`，c 即 classKey，多解拒绝）+ 循环头窗口
     （`iload i; aload_0; arraylength; if_icmpge`）。核窗口唯一命中才放行。
- **读取点三元组**：全类方法扫 `GETSTATIC <池>; <int idx>; AALOAD`——跨度
  内 NOP 跳过（squeeze 遗留落点不驻栈，number_fold 同口径）、含标签（合并
  点）即 fail-closed；下标非常量 = 非本家三元组静默跳过（`idxMissing`）；
  池链失败的类整类记账（`arrayInitBlocked`/`decryptorBlocked` 桶），无前导
  的 String[] 字段类（Kasumi 缓存同形）静默跳过。
- **逐下标解密**（S1 定案）：cipher（literal 走 MUTF-8 解码到 u16 码元流，
  孤立代理 fail-closed；arrayed 码元直取）→ `gruntCharXorStream` 复算明文 →
  记入 `Site{uri, methodIndex, anchorSrcPc, owner, fieldName, index, form,
  plainUnits, plaintext}`。

## Rewrite

- 按 uri 聚类 + `roundTripSafe` 前置门 + `mutateMethodWithFrames` 帧链写回
  （number_fold / string_cache_fold 骨架同源）。
- **锚点原地换**：三元组头三条替换为 `ldc <明文>`（`CodeEditor::loadConst`
  按索引选 LDC/LDC_W），`srcPc` 保留——pcmap/调试行落点不变。
- **明文落池 MUTF-8 转码**：NUL 走 `C0 80`、增补平面拆代理对各 3 字节、
  u16→UTF-8 记录形与 MUTF-8→u16 互逆面各有独立实现；挂续字节/孤立代理整站
  拒绝。CP 先 `findUtf8`/线性扫 String 复用，miss 才 `addUtf8`/`addString`。
- 池字段/arrayInit/解密器**不在此删**：供给链仍被 clinit 前导引用而存活
  （运行期空转填池，无读方），整体摘除属压池面的后续批次。失败粒度 = 类：
  帧链/编码失败整类放弃（`failed`）。

## Proof

`pass_clock::Suspend` + 类数不增 + 脏类可重新解析（`proof_tools::
classesIntact`，与全部残留同门）+ **residual**：改写后容器里池链仍成立且
读取点仍为三元组的站点数（scan 同判据 countOnly 重算）。语料单罐门要求
residual = 0。

## 语料账本（单罐验收）

- **来源**：`D:/WorkSpace/Grunt/samples/boar-main.jar`（Grunteon v3 全链
  产物，config `samples/config-on_260620.json`：StringBasicEncrypt + 参考
  混淆全开；jar 不进仓）。run-lab 无 grunt 罐（SnakeGame-obf 是 Kasumi 专
  链样本）。
- **手工普查**（mjt-mcp 独立线性扫描）：解密器形态方法 724、
  `String[]` static 池字段 725、aaload 总数 19905。
- **本仓实测**（grunt_cli 单跑 `grunt.StringPoolFold`，`GRUNT_TRACE=1`）：
  `scan: pool-classes=0 triple-hits=0 sites=0 rejects=724 (arrayinit=724
  decryptor=0 idx-miss=0) skip-classes=0`，全部 724 个池类落 **arrayInit
  桶**，原因同源：clinit 的 `invokestatic <arrayInit>` 被 Grunteon 全链的
  ReferenceObfuscate 藏进 indy 蹦床（helper 体 `[invokedynamic; return]`），
  蹦床跟随的 indy 形态 fail-closed 拒绝——**这是正确行为不是缺陷**：S1 判据
  要求供给链静态在场，调用目标藏在加密 BSM 里时证据链不完整，不猜。
- **上游依赖事实**：解开该 indy 需 IndyDecode（BSM 形 `(L, String, MethodType,
  String, String, String, Integer)` 加密参数），其 Era 门对本罐判
  `Unknown`（era.hpp 判据缺口 #15，不在本批面内）—— Era 门放行后
  indy_decode 还原蹦床为 `[invokestatic <ai>; return]`，本 pass 的直线蹦床
  跟随即可接通（unittest `FoldsThroughStaticTrampoline` 实证该形可折）。
- **门结果**：`corpus-regress.sh CORPUS=<boar 两罐> PASSES=<18 pass 全链>`
  —— 类数不增、VerifyJar 输入/输出失败集合逐类相同（混淆产物原生不可验证
  基线同挂，无新增失败）、exit 0。折叠计数 0 与手工普查对账：724 个池类
  全部因 indy 蹦床 fail-closed，无遗漏（725 字段 vs 724 类：1 个字段所在类
  无完整前导，静默跳过桶，trace `skip-classes` 口径）。
- **fail-closed 实证**（unittest 侧，12+1 例）：下标非常量/池字段形态不符/
  解密器缺失/seed 缺失/码元乱序/init 歧义/核窗口缺失 → 不动且字节逐位保持。

## 已知边界

- indy 藏面（本罐全量）依赖上游 indy 还原；本 pass 不评 BSM（基建边界，
  不自行扩）。
- arrayInit 多前导/多解密器/init 多解等歧义面全部 fail-closed 整类拒绝。
- 池读取点只钉**紧邻常量下标**三元组；经局部变量/方法调用间接取下标的
  形态不在面内（Grunt 生成面不产此形）。
- interface 池类（PUBLIC+FINAL 字段变体）未钉 access 差异——desc/owner/
  static 已足够定案，access 面留待有样本时再收窄。
