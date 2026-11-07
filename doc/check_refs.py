#!/usr/bin/env python3
"""代码引用面门禁：口径抄自 mjbase/libjbc 家族，按 grunt 迁移期改参。

只认四类可校验事实，其余一律失败：

1. ``doc:<label>`` 必须能在 doc/**.rst 里找到 ``.. _<label>:`` 声明；同理
   ``:ref:``/``:doc:`` 目标在 RST 侧自检（label 唯一 + doc 文件在册）。
2. 代码面出现的 ``doc/...rst`` 路径必须存在。
3. 文档/代码面不得点名「设计内容形态」的 ``.md``。白名单：流程文档
   （README/CONVENTION/CHANGELOG 等）；跨仓出处必须带仓名标记（mjt /
   morpher / grunt2 / grunt3 / luiox 等）。**迁移期特例**：本仓正处
   md→rst 逐批迁移中，点名「仓库里仍存在」的 md 文件放行——仓内账本
   简写（裸文件名 / pass 名下划线写法）按 doc/ 树内同名唯一命中解析
   （含下划线↔连字符归一）；文件删除/迁移后 mention 自动变红，倒逼
   逐批清理；源码注释 ``doc:<label>`` 指向铺开是后续批，本批不强制。
   jar 资源名（META-INF/**.md）是测试夹具字符串不是文档点名，豁免。
4. RST 里「``X`` 头注」指向微观层：``X`` 必须是某个 ``@defgroup`` 组名或
   某个头文件名。宏观规格不许指向不存在的组注释——那一半内容就失踪了。
   另：``doc_ai`` 在本仓从无本地目录，出现即指兄弟研究仓，必须带仓名
   标记，否则按坏引用计。

用法：python doc/check_refs.py [仓库根]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# 代码面：源码、测试、构建脚本。人读文档（README/CONVENTION）不在此列，
# 它们引用 .md 是允许的。
CODE_ROOTS = ("src", "test", "xmake.lua")
CODE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".lua", ".sh", ".py"}

# 流程文档：仓库级、稳定、允许被代码注释点名，但只校验存在性。
PROCESS_DOCS = {"README.md", "CONVENTION.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md"}
# 跨仓出处的 prose 标记：命中则该行的 .md 提及不参与存在性校验。
UPSTREAM_MARKERS = (
    "libjbc", "mjbase", "libca", "luiox", "mjt", "morpher",
    "grunt2", "grunt3", "Gruntpocalypse", "Grunteon", "Grunt 仓",
    "Kasumi", "私有仓", "私仓", "待迁",
)
# jar 资源路径前缀：测试夹具里的资源字符串，不是文档点名。
RESOURCE_PREFIX = "META-INF"

DOC_DIRNAME = "doc"
LABEL_DEF = re.compile(r"^\.\. _([\w.-]+):\s*$")
LABEL_REF = re.compile(r"doc:([a-z][a-z0-9]*(?:-[a-z0-9]+)*)")
RST_PATH = re.compile(r"doc/[\w./-]+\.rst")
LEGACY_PATH = re.compile(r"doc_ai")
MD_MENTION = re.compile(r"[\w./\\-]*[\w-]\.md\b")
REF_ROLE = re.compile(r":ref:`([^>`]+)`")
DOC_ROLE = re.compile(r":doc:`([^>`]+)`")
ANON_LINK = re.compile(r"`[^`]*\.(?:md|rst) <([^`>]+\.(?:md|rst))>`_{1,2}")
HEADNOTE = re.compile(r"([A-Za-z_][\w.-]*)(?:\{[\w-]+\})? 头注")
DEFGROUP = re.compile(r"@defgroup\s+([\w.-]+)")


def micro_targets(src_root: Path):
    """微观层可被「X 头注」点名的锚点：@defgroup 组名 + 头文件名/.stem。"""
    groups, headers = set(), set()
    if not src_root.is_dir():
        return groups, headers
    for p in src_root.rglob("*.hpp"):
        headers.add(p.name)
        headers.add(p.stem)
        for _, line in iter_lines(p):
            m = DEFGROUP.search(line)
            if m:
                groups.add(m.group(1))
    return groups, headers


def code_files(root: Path):
    for spec in CODE_ROOTS:
        base = root / spec
        if base.is_file():
            yield base
        elif base.is_dir():
            for p in base.rglob("*"):
                if p.is_file() and p.suffix in CODE_SUFFIXES:
                    yield p


def iter_lines(path: Path):
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    return enumerate(text.splitlines(), 1)


def md_allowed(root: Path, mention: str, line: str) -> bool:
    """迁移期放行规则：见模块 docstring 第 3 条。"""
    rel = Path(mention.replace("\\", "/"))
    if rel.name in PROCESS_DOCS:
        return True
    if any(marker in line for marker in UPSTREAM_MARKERS):
        return True
    if RESOURCE_PREFIX in str(rel):
        return True
    if (root / rel).exists():
        return True
    # 仓内 doc 账本简写放行（迁移期）：裸文件名 / pass 名下划线写法按 doc/
    # 树内同名唯一命中解析（含下划线↔连字符归一）；命中不唯一或零命中仍失败。
    hits = set()
    for name in {rel.name, rel.name.replace("_", "-")}:
        hits |= {p for p in (root / DOC_DIRNAME).rglob(name) if p.is_file()}
    if len(hits) == 1:
        return True
    return False


def collect_labels(doc_dir: Path):
    labels, files, dup = {}, set(), []
    for p in sorted(doc_dir.rglob("*.rst")):
        files.add(p.name)
        for lineno, line in iter_lines(p):
            m = LABEL_DEF.match(line)
            if m:
                name = m.group(1)
                if name in labels:
                    dup.append(f"{p.name}:{lineno}: label 重复定义 {name}（首见 {labels[name]}）")
                labels[name] = f"{p.name}:{lineno}"
    return labels, files, dup


def toctree_entries(index: Path):
    """index.rst 里 toctree  directive 的条目（去扩展名）。"""
    if not index.exists():
        return set()
    inside, entries = False, set()
    for line in index.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith(".. toctree::"):
            inside = True
            continue
        if inside:
            if stripped == "":
                continue
            if stripped.startswith(":"):
                continue
            if not line.startswith("   "):
                inside = False
                continue
            entries.add(stripped.split("#")[0].strip())
    return entries


def check(root: Path):
    problems = []
    doc_dir = root / DOC_DIRNAME
    if not doc_dir.is_dir():
        return [f"{DOC_DIRNAME}/ 不存在：目录已改名或迁移未落地"]
    labels, rst_files, dup = collect_labels(doc_dir)
    problems += dup
    in_tree = toctree_entries(doc_dir / "index.rst")
    groups, headers = micro_targets(root / "src")

    # RST 侧：文档内链必须指向在册文件/已声明 label，匿名 md/rst 链接一律禁止。
    for p in sorted(doc_dir.rglob("*.rst")):
        if p.name == "index.rst":
            continue
        if p.stem not in in_tree:
            problems.append(f"{p.name}: 未挂进 index.rst 的 toctree（Sphinx -W 会报 orphan）")
    for p in sorted(doc_dir.rglob("*.rst")):
        for lineno, line in iter_lines(p):
            for m in LEGACY_PATH.finditer(line):
                if not any(marker in line for marker in UPSTREAM_MARKERS):
                    problems.append(f"{p.name}:{lineno}: doc_ai 提及须带仓名标记（兄弟研究仓）")
            for m in ANON_LINK.finditer(line):
                problems.append(f"{p.name}:{lineno}: 匿名文件链接 {m.group(1)}，改用 :ref:/:doc:")
            for m in MD_MENTION.finditer(line):
                if not md_allowed(root, m.group(0), line):
                    problems.append(
                        f"{p.name}:{lineno}: 文档点名 markdown 规格 {m.group(0)}——"
                        "在册 rst 用 :ref:/:doc:，跨仓出处加仓名标记"
                    )
            for m in REF_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and target not in labels and not target.startswith("<"):
                    problems.append(f"{p.name}:{lineno}: :ref:`{target}` 无对应 label 声明")
            for m in DOC_ROLE.finditer(line):
                target = m.group(1).split("<")[-1].strip().rstrip(">, ")
                if target and Path(target).name not in rst_files and Path(target).stem not in {f[:-4] for f in rst_files}:
                    problems.append(f"{p.name}:{lineno}: :doc:`{target}` 指向不在册文件")
            for m in HEADNOTE.finditer(line):
                target = m.group(1)
                if target in groups or target in headers:
                    continue
                problems.append(
                    f"{p.name}:{lineno}: 「{target} 头注」在微观层无锚点——"
                    f"应为 @defgroup 组名或头文件名（现有组：{', '.join(sorted(groups)) or '无'}）"
                )

    # 代码面：RST 路径与 doc:<label> 引用必须可解析；md 提及走迁移期放行规则。
    for f in code_files(root):
        rel = f.relative_to(root).as_posix()
        for lineno, line in iter_lines(f):
            for m in LEGACY_PATH.finditer(line):
                if not any(marker in line for marker in UPSTREAM_MARKERS):
                    problems.append(f"{rel}:{lineno}: doc_ai 提及须带仓名标记（兄弟研究仓）")
            for m in LABEL_REF.finditer(line):
                if m.group(1) not in labels:
                    problems.append(f"{rel}:{lineno}: doc:{m.group(1)} 无对应 label 声明")
            for m in RST_PATH.finditer(line):
                rel_path = m.group(0)
                if not (root / rel_path).exists():
                    problems.append(f"{rel}:{lineno}: 文档路径不存在 {rel_path}")
            for m in MD_MENTION.finditer(line):
                if not md_allowed(root, m.group(0), line):
                    problems.append(
                        f"{rel}:{lineno}: 代码面引用 markdown 规格 {m.group(0)}——"
                        "迁移期只放行仍存在的文件，跨仓出处加仓名标记"
                    )

    return problems


def main(argv):
    root = Path(argv[1]).resolve() if len(argv) > 1 else Path(__file__).resolve().parents[1]
    problems = check(root)
    if problems:
        print(f"文档引用门禁失败：{len(problems)} 处")
        for p in problems:
            print("  " + p)
        return 1
    print("文档引用门禁通过")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
