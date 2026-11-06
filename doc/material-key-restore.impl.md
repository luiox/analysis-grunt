# material_key_restore 实现笔记

## 数据流

`ScanInput{}`（无旁证注入面——判据全为容器自证事实）→
`ScanResult{sites, rejects, materialClasses, keyBodyMismatch, initBlocked,
initConflict, writerShape, writerUnguarded, keyRefuted,
decryptorAmbiguous, classesSkipped}` → `DirtySet{uris, sites, classes,
failed, skippedByRoundTrip}` → `ProofResult{ok, failed, residual}`。三段
签名与全部残留同形；Era 不消费（类级 Grunt BSM desc 指纹做家族锚，无
BootstrapMethods 指纹的类不进面）。

`Site{uri, decryptorMethodIndex, invokeSrcPc, decryptorRawKey,
materialKey, sitesVerified}`——`invokeSrcPc` 跨会话稳定，rewrite 靠它重
定位；`materialKey` 是折回值（有效 key = `K ^ materialKey` 的补数面，
`K` = 窗口 `decryptorRawKey`）。

## 算法（源锚点）

grunt3@e41e852：

- `RuntimeMaterial.kt` `createMaterialKeyMethod`（`materialKey()` 体逐字
  9 条）+ `clinitMaterialInit`（LDC2_W 常量）+ `perturbation`（clean
  delta 只碰 shareA/shareB；可疑路径守卫 `INVOKESTATIC guard; DUP; IFEQ`
  后写 keyField/poison）；
- `ReferenceObfuscate.kt`（材料解密器窗口 + `simpleDecryptKey =
  decryptKey xor seedFold` 构建期常量）；
- `Long.foldToInt`（`fold(K) = (int)(K ^ K >>> 32)`，java.lang.Long 生成
  面同式）。

boar-main.jar 逐字节互证（mjt-mcp method_jasm 级取证）：656 材料类、
70 类 `materialKey()` native 化、每类恰一解密器且 `materialKey()` 恰一
调用点、CFF 摊平块头标签在窗口头。

## Scan（单类 fail-closed 整类拒）

- **家族锚**：类级 Grunt BSM desc 指纹（simple 7 参）在册才进面；
- **`promoteAllInto`**：全类方法提升为 MethodView（fill-missing；任一失
  败 → 拒，提升失败的类连坐）；
- **窗口定位** `findMaterialWindows`：`charAt; LDC K; INVOKESTATIC <self>
  ()I; IXOR; IXOR; I2C` 回溯，多窗/`materialKey()` 多调用点 →
  `decryptorAmbiguous`；
- **mk 体钉形** `matchMaterialKeyBody`：9 条逐字（GETSTATIC kf J; DUP2;
  INT 32; LUSHR; LXOR; L2I; GETSTATIC po I; IXOR; IRETURN）+ 字段
  private static；native/形异 → `keyBodyMismatch`；
- **调用点普查**：`materialKey()` 全容器引用恰为 1（材料解密器窗口）；
- **clinit 定位 + 字段普查** `FieldCensus`：keyField/poison 各恰一初值
  写且在 clinit（多写 → `initConflict`；写不在 clinit → 同桶）；
  其余写全为扰动窗口形（头 GETSTATIC 允许带标签；窗口中段标签/分支/
  同字段写 → `writerShape`；窗口尾 LXOR|IOR）；
- **守卫指纹** `guardFingerprint`：扰动写所在方法带
  `INVOKESTATIC <self> ()I; DUP; IFEQ` 连续三条，缺 → `writerUnguarded`；
- **初值求值** `evalKeyInit`：Form A `ldc2_w` 直连 / Form B 五跨
  （`ldc <基32>; bipush 32; INVOKESTATIC parse|蹦床 (String;I)J; ldc2_w
  C; LXOR`）——radix 恒 32 严格解析，`parseUnsignedLong` 语义 C++ 直解
  （固定 JVM 语义，与 NumberFold 基 32 面同口径）；池读在场 →
  `initBlocked`（why 报链序缺 `grunt.StringPoolFold`）；`evalPoisonInit`：
  受限迷你栈机（常量装载/转换/整长算术白名单，try-all-starts 窗 24，
  唯一值 + 顶型 I 才放行）；
- **候选 key**：`materialKey = fold(keyInit) ^ poisonInit`；
- **S1 互证**：BSM 名字族路由（BSM 名 B[2, len-1) == 解密器名）+ 体跟随
  兜底（BSM 体内 INVOKESTATIC 解密器）→ 全部路由 indy 站点按
  `effectiveKey = rawKey ^ materialKey` 解密 encOwner/encName/encDesc
  （自逆 XOR）+ desc 交叉互证（static 参数段全等 / virtual 首参
  Object + 其余逐位一致）；有站点全过 → `sitesVerified` 记数；有站点
  任一失败 → `keyRefuted` 整类拒；窗口无路由站点 → 结构 S2 成立
  （`sitesVerified=0` 如实在账）。

## Rewrite

只消费 ScanResult：按 `invokeSrcPc` 重定位（新布局 pc 与 srcPc 对表），
复核（pos 界内、`order[pos]` 恰为 INVOKESTATIC mk）后原地换 `LDC
<materialKey>`（makeConstLoad I32，srcPc 保留）；FrameContext 帧链正统
重算 + `roundTripSafe` 往返门 + `mutateMethodWithFrames` 写回。拒绝面
字节逐位不动（单测覆盖：容器 memcmp 全等）。

## Proof

类数不增（classesBefore 对照）+ 脏类可重新解析 + residual 统计（改写后
容器里仍带材料解密器窗口形的类数——语料单罐门要求 0）。

## 语料账本（boar-main.jar）

- **r1 供给链**（`grunt.IndyRestore,grunt.StringPoolFold` 产物罐）：
  materialLane 拒绝桶 42228 站（boar 主罐最大余量），656 材料类；
- **本 pass 单跑 r1**：586/656 类成立（58 窗口无路由站点
  `sitesVerified=0` 结构成立，528 类 37168 站点 S1 全过、keyRefuted=0）；
  拒绝 70 类全数 `keyBodyMismatch`（native materialKey 硬余量，如实在
  账）；首轮修复记录：CFF 摊平块头标签曾误拒全量（窗口头标签放行后
  归零）；
- **组合链**（`grunt.IndyRestore,grunt.StringPoolFold,
  grunt.MaterialKeyRestore,grunt.IndyRestore`）：materialLane 42228 →
  5060（37168 站点第二轮联动折叠；余 5060 = 70 native 类的站点，静态
  天花板）；`-Xverify:all` 逐类失败集与输入全等、类数 3065 不增、双跑
  逐位一致（见 README 残留表行语料注）。

## 单测（unittest/material_key_restore_test.cpp）

jasm 现场汇编夹具（缩小版 RuntimeMaterial 形态：字段 kf J/po I + mk()
9 条逐字 + Dec 材料解密器 + tramp/gd/BSM/caller；初值链 boar 逐形同
源——Form B 基 32 + 蹦床、poison X^X 走廊、守卫支路扰动窗）。8 测试：

- **真值**：`TrueValueRoundtripFoldsMaterialKey`（链求值 → 候选 =
  `fold(parseUnsignedLong(M,32) ^ C) ^ 0`，S1 互证 `sitesVerified=1`）；
- **S1 咬合反证**：`S1RefutesWrongCandidateKey`（poison 求值面抬高 →
  候选 ≠ 真钥 → 互证解出垃圾 → keyRefuted + 字节逐位不动）；
- **拒绝面**：`PoolFedInitChainBlocked`（池读未折回 → initBlocked 报
  StringPoolFold）/ `DoubleKeyInitConflicts`（initConflict）/
  `ExternalWriteRejected`（非扰动形裸写 → writerShape）/
  `SuspiciousWriteWithoutGuardRejected`（扰动窗无守卫 →
  writerUnguarded）/ `NativeMaterialKeyBodyMismatch`（boar 70 类同形
  → keyBodyMismatch）——全部 rewrite 无脏类 + 容器字节逐位不动；
- **REWRITE**：`SwapsInvokestaticForLdcInPlace`（窗口 INVOKESTATIC →
  LDC，proof ok residual=0）。

jasm 汇编坑（记账）：`ldc2_w` 长字面量必须带 `L` 后缀——裸十进制被按
int 解析，`lxor` 求值即 stack underflow（帧分析报错点位在初值跨度，
极难归因；其他测试夹具均带后缀，本夹具初版漏写）。

## 已知边界

- native `materialKey()`（70/656 类）静态不可还原，永久 residual；
- 守卫命中路径的 keyField/poison 扰动保留原样（还原口径 = 清洁运行
  环境语义，见 material-key-restore.md）；
- `materialKey()` 本体/材料字段/守卫支路不删（折回后零引用，交
  DeadMember/DeadCode 回收）；
- 非 Form A/B 的初值跨度（其他算术走廊）fail-closed，等语料命中再扩
  （boar 全量已覆盖，无已挂账命中）。
