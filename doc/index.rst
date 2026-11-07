analysis-grunt 行为账与 pass 规格
=================================

Grunt 家族（v2 Gruntpocalypse / v3 Grunteon）混淆产物静态还原的行为账与 pass 规格权威文本。本目录收 RST：行为 × residue 状态总账、语料罐台账、复杂 pass 规格；逐面账本族（形态 / 实现笔记两类 markdown 账本）仍在 markdown 层，按批次逐篇迁移，迁移篇删 md、未迁篇原样保留。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；\ ``check_refs.py`` 校验这些指向不会腐烂（源码注释的 ``doc:`` 指向铺开是后续批）。下列文档的标题即为该文档的根 label。

.. toctree::
   :maxdepth: 2
   :caption: 行为账与规格

   obf-behaviors
   corpus-ledger
   gate-processor-fold-spec

待迁清单（md 层，后续批迁入本站）：

- ``doc/obf-behaviors/grunt-v2-behaviors.md``——v2（Gruntpocalypse 2.x）35 个 transformer 行为账。
- ``doc/obf-behaviors/grunt-v3-behaviors.md``——v3（Grunteon）39 个 transformer + GlslObfuscator 插件 + NativePipeline 阶段行为账。
- 逐面账本族（形态 / 实现笔记两类 markdown 账本，约 30 对），按批转 RST 或并入对应 pass 规格篇。
