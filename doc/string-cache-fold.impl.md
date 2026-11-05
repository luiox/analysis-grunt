# string_cache_fold 实现笔记

## 数据流

`ScanInput{era, plaintexts, tsvState}` → `ScanResult{sites, rejects,
totalShapeHits, eraBlocked, tsvBlocked, noPlaintext, classesSkipped}` →
`DirtySet{uris, rewrittenCacheReads, rewrittenDirectDecrypt, failed,
skippedByRoundTrip}` → `ProofResult{ok, failed, residual}`。**注入面**：TSV
文件读取收敛在 scan 之前——CLI `--tsv` 传路径，adapter
（`gKasumiTsvPath`）在 `probeEra` 判为 Kasumi 时才读文件、`parseTsv` 纯函数
解析（点分 owner → slash，转义反转义）；残留本体零文件系统副作用。TSV 内
容不进仓：doc 只记来源形态（KasumiStringDumper 四列 TSV），不记样本明文。

## Scan

- **粗找 = GETSTATIC 全量起锚**，细查 = `tryMatchAt`：memberOf 解析字段
  语义，desc 钉 `[Ljava/lang/String;`（形态 1）/ `[[B`（形态 2）
  （knowledge 表，树版字面量），后继非 NOP 指令逐条钉形（int 常量装载族
  ICONST_M1..5/BIPUSH/SIPUSH/LDC Integer、AALOAD、DUP、ARRAYLENGTH、
  INVOKESTATIC desc `([BII)Ljava/lang/String;`）。跨度是**前缀钉面**——
  形态 1 到 aaload、形态 2 到 invokestatic 为止，后继与本站无关。
- **NOP 跳过**：squeeze 遗留落点不驻栈（number_fold 同口径）；**标签门**：
  锚点之后到尾条之间任何带标签指令 = 合并点，整站放弃。
- **三级分工**：S2 形状钉面（`totalShapeHits`）→ S3 Era 门
  （`Era::Kasumi` 未判整站拒绝，`eraBlocked`——TSV 是换代表面，Era 分支
  唯一合法落点）→ 旁证降级门（`tsvState != Loaded` 整站拒绝，
  `tsvBlocked`：Absent = CLI 未注入、Unreadable = 文件缺失/不可读）→
  S1 明文表互证（`(owner, fieldKey, idx)` 精确命中 = 站点定案；无命中 =
  `noPlaintext`，**树版静默跳过对齐**——dumper 只导出运行期缓存非空的
  槽位，覆盖是部分的，缺席不是异常）。
- **S1 无旁证不可定案**（树版对齐结论）：树版 `parseStringDump` 空映射时
  整个 pass 静默返回；站点查表未命中时 `continue` 跳过。两态都不改写——
  本仓把「无旁证」显式记账（rejects 进 trace），改写面语义与树版逐位一致。

## Rewrite

- 按 uri 聚类 + `roundTripSafe` 前置门 + `mutateMethodWithFrames` 整体帧链
  重算（number_fold 骨架同源）。
- **锚点原地换**：`ldc <明文>`（`CodeEditor::loadConst` 按索引选 LDC/
  LDC_W），`srcPc` 保留——pcmap/调试行落点不变。**跨度其余非 NOP 指令
  eraseBatch**（kRejectIfUsed：受害者被异常表引用即拒收——回滚锚点整站
  放弃，不留半改写态）。NOP 遗留落点不动，DeadCode 收尾。
- **CP 先 intern 再动 DOM**：Utf8 `findUtf8`（命中既有池不膨胀）→ miss 才
  `addUtf8`；String 包装先线性扫池复用，miss 才 `addString`。0 = 失败哨兵。
- **明文 MUTF-8 转码**：TSV 是 UTF-8，常量池 Utf8 是 modified UTF-8——
  NUL 走 `C0 80`、增补平面拆代理对各落 3 字节、过短编码/挂续字节整站拒绝
  （旁证是外部注入，不猜）。
- 失败粒度 = 类：帧链/编码失败整方法放弃；形态分列计数只在类写回成功后
  归账。

## Proof

`pass_clock::Suspend` + 类数不增 + 脏类可重新解析（`proof_tools::
classesIntact`，与全部残留同门）+ **residual**：改写后容器里仍「全形 +
明文表命中」的站点数（`countResidual` 与 scan 同判据重钉）。旁证缺席的
形状命中**不进** residual——那是覆盖面事实（运行期缓存为空的槽位 dumper
导不出来），不是还原缺陷。语料单罐门要求 residual = 0。

## 语料账本（单罐验收）

- **来源**：`D:/WorkSpace/run-lab/SnakeGame-obf.jar`（= morpher 树版样本
  SnakeGame-obf (1).jar，账本同源；jar 不进仓）。
- **旁证**：`KasumiStringDumper`（morpher tools/ 同源）对该罐运行产出，
  222 行（7 类 × {a,b,c, byte:a, byte:b} 非空槽位）；运行时 `--tsv` 注入，
  内容不进仓。
- **形状普查**（独立 Python 线性扫描旁证）：形态 1 = 86、形态 2 = 84。
- **本仓实测**（grunt_cli，`GRUNT_TRACE=1`）：scan `era=Kasumi tsv=loaded
  total-shape=170 sites=168 rejects=0 no-plain=2`；rewrite `classes=7
  sites=168 failed=0 roundtrip-skip=0 (cache-read=84 direct-decrypt=84)`；
  proof `ok residual=0`。
  - **形状口径差**：Python 普查 form1=86 vs 本仓 shape=84+2noPlain——
    本仓把 2 个 `h.a` 站点判为 noPlaintext（运行期缓存槽为 null，dumper
    按「非 null 才导出」漏掉，表查不到即树版静默跳过），两者一致：形状
    命中 170 = 84 + 84 + 2。
- **门结果**：类数 25 → 25；VerifyJar 输入/输出均 rc=0、失败集逐类同
  （双空）；语料回归 `corpus-regress.sh ONLY=SnakeGame-obf
  PASSES=grunt.StringCacheFold --tsv …` = ok。
- **fail-closed 实证**（同罐）：不带 `--tsv`（`tsv=absent`）与传不存在路
  径（`tsv=unreadable`）均 `sites=0 rejects=170`（全部落 tsvBlocked 桶），
  不产生任何改写站点。

## 已知边界

- char[][] 密文载体（`a([CII)` 解密器）不在面内——树版同样只处理 byte[][]
  形态 2 与 String[] 形态 1；样本未见 char 站点。
- 明文表是**运行期快照**：缓存槽为 null 的下标旁证缺席（样本 h 类 2 站），
  树版语义 = 跳过，本仓 = `noPlaintext` 计数。
- 换代面：TSV 列格式 / `byte:` 前缀 / 两形态描述符都钉自当前 Kasumi 版本，
  换代由 Era 门兜底整站拒绝。
- 形态 2 的解密方法 owner 不与本站 getstatic owner 互钉（树版同口径）——
  数据流由栈形锁定，表键只认 getstatic 侧 owner/field/idx。
