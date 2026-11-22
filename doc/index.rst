analysis-grunt 行为记录与 pass 规格
========================================================

Grunt 家族（v2 Gruntpocalypse / v3 Grunteon）混淆产物静态还原的行为记录与 pass 规格权威文本。本目录收 RST：行为 × residue 状态总表、语料 jar 统计、逐面形态文档（每注册 pass 一篇：混淆器产出什么结构 + 本仓怎么还原 + 验证实证）、复杂 pass 规格与端到端验收记录。各 pass 的实现层细节（结构体字段、Scan / Rewrite / Proof 接口契约）在各 pass 源码文件头注释。

口径分工：

- 逐面文档写三件事：混淆器会产出什么结构（换个输入 jar 仍然成立）、本仓的还原判据与改写契约（Scan / Rewrite / Proof）、验证实证（unittest 面 + 真实语料 trace 数字）。实现层细节在 `src/grunt/<pass>.cpp` / `<pass>.hpp` 的 `///` 面（Doxygen 检查覆盖）。
- 具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
- 例外：kasumi-verify 是专链端到端验收记录（五项检查数字 + 口径差异 + 通用链取证），只记数字与来源，语料 jar / TSV / 产物不进仓。

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
   kasumi-verify

.. toctree::
   :maxdepth: 1
   :caption: 逐面 pass 文档

   arith-fold
   bait-strip
   booger-anno-strip
   cff-unflatten
   concat-restore
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
   junk-member-strip
   junk-region-strip
   key-proc-eval
   material-key-restore
   metadata-strip
   noise-attr-strip
   number-fold
   pool-array-fold
   predicate-fold
   proxy-inline
   seed-strip
   string-cache-fold
   string-equals-restore
   string-pool-fold
   trampoline-squeeze
