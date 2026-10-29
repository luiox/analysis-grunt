# trampoline_squeeze 实现笔记

## 数据流

`Site{uri, methodIndex, srcPc, wide}` → `DirtySet{uris, squeezed, attempted}`。

- SCAN：逐方法 promote 后按 order 遍历，`resolveLabel(in->target)` 的句柄
  与 `order[i+1]` 相等即收站点。
- REWRITE：按 uri 聚合 → 逐方法一次 promote，按 `srcPc` 重定位站点，**复核
  判据**（同方法其他站点改写后布局可能变，句柄判据重推）→ `*m = ir::Insn{}`
  清干净后置 `NOP` + 保 `srcPc` → 逐方法 `lowerMethod`（失败整类放弃）→
  `writeClass` → `replaceClass`。
- PROOF：`proof_tools::classesIntact`。

## 为什么 NOP 而不是擦除

NOP 化零布局扰动：帧/行号锚不动，不触发 lower 的属性重定位失败路径。缩
布局的收益留给 dead_code（非 indy 擦除）统一拿。

## 已知边界

目标在方法末尾之外（`t.isEnd()`）不是蹦床，跳过。
