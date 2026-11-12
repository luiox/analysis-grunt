"""Sphinx 配置：analysis-grunt 行为账与 pass 规格层。

规范见家族口径（mjbase/libjbc doc 站同款）：本目录收 RST 行为账与 pass 规格；
markdown 属人读/AI 方向层（逐面 ``<residue>.md`` 行为描述），实现说明在各 pass
源码文件头 /// 注释，不进 doctree，markdown 按批次逐篇迁移。CI 以 -W
--keep-going 构建，警告即失败。
"""

project = "analysis-grunt"
author = "analysis-grunt maintainers"
copyright = "2026, analysis-grunt maintainers"

# 中文正文：docutils 会把全角字形算进表格边框宽度，因此本仓禁用 grid table，
# 表格一律用 list-table（无对齐不变量）。见家族写作约定。
language = "zh-CN"

root_doc = "index"

exclude_patterns = ["_build", "_apidoc", "Thumbs.db", ".DS_Store"]

# 引用必须可解析：未定义的 :ref:/:doc: 目标在 -W 下即失败。
nitpicky = True
nitpick_ignore = [
    # 行为账里点名的 C++/pass 符号不做跨域解析（微观契约归 Doxygen 层）。
    ("py:class", "doc"),
]

html_theme = "alabaster"
html_title = "analysis-grunt 行为账与 pass 规格"
html_baseurl = ""
html_search_language = "zh"  # 需 jieba；未装分词器时仅影响站内搜索，不影响门禁
html_use_opensearch = ""

# 站点内跳转锚点与 label 同名，便于代码注释里的 doc:<label> 人工回溯。
html_copy_source = True
html_show_sourcelink = True
