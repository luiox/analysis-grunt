analysis-grunt 行为账与 pass 规格
=================================

Grunt 家族（v2 Gruntpocalypse / v3 Grunteon）混淆产物静态还原的行为账与 pass 规格权威文本。本目录收 RST：行为 × residue 状态总账、语料罐台账、复杂 pass 规格；逐面形态文档（每个残留一篇，混淆器会产出什么结构）仍在 markdown 层，按批次逐篇迁移，迁移篇删 md、未迁篇原样保留。各 pass 的实现说明（Scan / Rewrite / Proof 判据与已知边界）在各 pass 源码文件头注释，不占文档层。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；\ ``check_refs.py`` 校验这些指向不会腐烂（源码注释的 ``doc:`` 指向铺开是后续批）。下列文档的标题即为该文档的根 label。

.. toctree::
   :maxdepth: 2
   :caption: 行为账与规格

   obf-behaviors
   obf-behaviors/grunt-v2-behaviors
   obf-behaviors/grunt-v3-behaviors
   corpus-ledger
   gate-processor-fold-spec

待迁清单（md 层，后续批迁入本站）：

- 逐面形态文档（约 24 篇 markdown），按批转 RST 或并入对应 pass 规格篇。
