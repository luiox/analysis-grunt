analysis-grunt 行为记录与 pass 规格
========================================================

Grunt 家族（v2 Gruntpocalypse / v3 Grunteon）混淆产物静态还原的行为记录与 pass 规格权威文本。本目录收 RST：行为 × residue 状态总表、语料 jar 统计、逐面形态文档（每残留一篇，写混淆器会产出什么结构）、复杂 pass 规格与端到端验收记录。各 pass 的实现说明（Scan / Rewrite / Proof 判据与已知边界）在各 pass 源码文件头注释，不占文档层。

口径分工：

- 形态文档只写混淆器会产出什么结构；换个输入 jar 仍然成立的内容才写。
- 本仓怎么还原（固定三章：Scan / Rewrite / Proof，含判据分级、改写契约与已知边界）在各 pass 源码文件头注释：`src/grunt/<pass>.cpp` / `<pass>.hpp` 的 `///` 面（Doxygen 检查覆盖）。
- 具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
- 唯一例外：kasumi-verify 是专链端到端验收记录（五项检查数字 + 口径差异 + 通用链取证），只记数字与来源，语料 jar / TSV / 产物不进仓。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；\ ``check_refs.py`` 校验这些指向不会腐烂。下列各文档的标题即其根 label。

.. toctree::
   :maxdepth: 2
   :caption: 行为记录与规格

   obf-behaviors
   obf-behaviors/grunt-v2-behaviors
   obf-behaviors/grunt-v3-behaviors
   coverage
   corpus-ledger
   gate-processor-fold-spec

.. toctree::
   :maxdepth: 1
   :caption: 逐面形态文档

   arith-fold
   booger-anno-strip
   cff-unflatten
   constant-call
   const-method
   const-pool-restore
   const-value-promote
   dead-code
   dead-member
   dispatcher-restore
   exc-bridge-restore
   field-access-proxy
   flags-strip
   indy-decode
   indy-fold
   indy-restore
   kasumi-verify
   material-key-restore
   metadata-strip
   number-fold
   predicate-fold
   proxy-inline
   seed-strip
   string-cache-fold
   string-equals-restore
   string-pool-fold
   trampoline-squeeze
