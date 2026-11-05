# metadata_strip 实现笔记

## 数据流

`ScanInput{era}`（adapter 调 `probeEra` 一次注入）→ `ScanResult{sites,
resources, totalAnnos, totalFields, resourceCandidates, annoBlocked,
eraBlocked, classesSkipped}` → `DirtySet{uris, removedResources, classes,
annos, fields, failed, annoBlocked, resourceCandidates}` → `ProofResult{ok,
failed, residualAnnos, residualFields, residualResources}`。无外部注入面：
三面判据全部内证（注解描述符 / 水印串形状 / 资源形状 + 内容探针），零
文件系统副作用。

## Scan

- **S3 Era 门**：`era != Kasumi` 即整面关闭（`eraBlocked`，fail-closed）——
  注解描述符、水印文本、资源前缀与探针表全是 Kasumi 当前版本的实现选择，
  Era 分支是它们唯一合法落点。未判时一个字节都不看内容判据（语料实证：
  非 Kasumi 罐挂同前缀资源 → scan 空、CLI 产物里资源逐字节原样）。
- **注解面**：`countMetadataTable` 走 decodeAttr 直调位（booger_anno_strip
  同一通路），类/字段/方法三张挂载面普查 `@Metadata` 条目。深度预检
  （`attrSafeToDecode`，mjbase #158）拦下的槽计 `annoBlocked`——解不开的
  fail-closed 不摘、不猜条目数。
- **水印字段面**：字段带 `@Metadata` 或 ConstantValue 为水印串（树版
  isKasumiWatermarkString 三判据）都计入 `fields`——字段面命中即整字段
  删除。ConstantValue 双形态都收：jbc 原生 `ConstantValueAttr` 解码产物与
  `RawAttr` 2 字节载荷（const_value_promote 新建形态）。
- **资源面**：`visitResources` 全量走一遍，只看 `UnknownResource` 载荷
  （树版只看 Unknown 同口径）。URI 前缀 + `.md` + 内容探针三证齐 → 删除
  清单；形状命中而证据不全 → `resourceCandidates` 盘点不删（树版返回
  false 同口径——前缀面可能有合法文档，删的是内容坐实的注入物）。

## Rewrite

- **资源先删**（树版序）。只消费 scan 清单，删除时刻复核同判据（scan 之后
  字节可能已变，不盲删——K2 rewriteSite 复核同纪律）；复核不齐计
  `resourceCandidates`。删除动词 = `rewrite_tools::removeResource`（本仓
  首个资源面写原语：容器 map 摘条目，URI arena 文本不回收）。
- **类清理**（booger_anno_strip 同骨架）：类面/方法面只摘条目（摘空属性槽
  整槽移除）；字段面摘条目后按「摘到过条目 ∨ ConstantValue 水印」整字段
  删除（树版 removeKasumiWatermarkFields 对齐：注解字段与水印字段都是
  混淆器署名位，直接删）。方法体不碰。
- **压池收尾**：`classAnnoDepthSafe` 预检 + `compactConstantPool` 带走死
  条目（注解描述符 / 水印串 / 字段引用）；预检拒绝或压缩失败就跳过压缩，
  清理过的字节照常落盘——CP 不压缩只是少带走死条目（booger 同口径）。
- **条目数在类写回成功后归账**；编码失败计 `failed`（未归账如实记账）。

## Proof

`pass_clock::Suspend` 挂起全量，三道门：

1. **类数门**（`classesIntact`，与全部残留同门）：类数不增 + 脏类可重新
   解析；
2. **资源对账门**（`resourcesShrunk`，proof_toolbox 新收口）：资源数不增
   （只减）+ 删除清单逐条不在场——虚报删减即 FAILED；
3. **三面 residual**：与 scan 同判据重钉（注解条目 + 水印字段 + 待删资
   源），语料单罐门要求全 0。深度预检拦下的敌意槽不在 residual 视野内
   （不可解面不是清理缺陷，scan 的 annoBlocked 已记账）。

## 语料账本（单罐验收）

- **来源**：`D:/WorkSpace/run-lab/SnakeGame-obf.jar`（与 indy-decode /
  string-cache-fold 账本同源；jar 不进仓）。
- **三面命中**（独立 Python 线性扫描旁证 + 本仓 trace 双向印证）：原始字
  节面 `Ldev/kasumi/internal/Metadata;` 25 类各 1；`PROTECTED BY Kasumi`
  34 处 / `Gruntpocalypse` 9 处 / `kasumi_` 前缀串 7 个；反 LLM 资源 4 个。
- **本仓实测**（grunt_cli，`GRUNT_TRACE=1`）：scan `era=Kasumi classes=25
  annos=25 fields=10 resources=4 candidates=0 anno-blocked=0`；rewrite
  `classes=25 annos=25 fields=10 resources=4 failed=0`；proof `ok
  residual(a/f/r)=0/0/0`。
  - **口径差（树版对齐）**：原始字节 50 处水印串命中 vs 字段面删除 10——
    其余水印串在方法体常量面（引导参数 / 解密器体），树版本 pass 不触方
    法体（头注明示），我们如实对齐；注解条目 25 = 字节面 25（无一在
    TypeAnnotations / ParameterAnnotations 面上）。
  - **CP 残字边界**：25 类里 9 类压池成功带走死描述符 Utf8，16 类压缩被
    封闭集拒绝（厂商/未知属性非零长 Raw 体 → `AttrNotModeled`，indy 类
    集中）保留死 Utf8——注解条目已摘（residual=0 按条目判据），死串惰性
    不引用，类字节合法（booger_anno_strip 同一口径：CP 不压缩只是少带走
    死条目）。
- **门结果**：类数 25 → 25；条目 30 → 26（恰删 4 个资源）；VerifyJar 输
  入/输出 25/25 ok 失败集逐类同（双空）；`corpus-regress.sh
  ONLY=SnakeGame-obf PASSES=grunt.MetadataStrip` = ok。
- **fail-closed 实证**：非 Kasumi 罐（仅 MANIFEST + 同前缀 .md 资源，零类）
  `era=Unknown resources=0`，产物里 `anti-llm` 资源逐字节原样；清理后罐复
  跑（幂等）scan 全零。
- **组合链去风险**（K4 预演）：`grunt.IndyDecode,grunt.MetadataStrip
  --jdk <jdk17>`：627 indy 站点全还原（K1 账本复现）+ 三面全清，VerifyJar
  25/25 ok。

## 已知边界

- 水印串清理只到**字段面**：方法体内的水印常量（含 `PROTECTED BY Kasumi`
  字面量与 `kasumi_` 前缀会话 key）是 indy 链路运行时数据，树版不触方法
  体，本仓对齐；全链收口后由 DeadMember 压池/死码面自然带走解引用的死串。
- 水印字段删除不做引用普查（树版直接删）：若未来样本出现「业务代码引用
  水印字段」的病态形态，VerifyJar 会拦（类加载校验）；当前语料零引用。
- 深嵌敌意注解槽 fail-closed 不摘（`annoBlocked` 记账），该槽里的
  `@Metadata` 不进 residual——不可解面，不是清理缺陷。
- 换代面：注解描述符 / 水印三判据 / 资源前缀与五探针都钉自当前 Kasumi
  版本，换代由 Era 门兜底整面拒绝。
