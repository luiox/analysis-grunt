# seed_strip 实现笔记

## 数据流

`Site{uri, fieldName, spanCount}` → `DirtySet{uris, fields, attempted,
failed}`。

## 栈深模拟

`spanDepthDelta(op)` 词汇表逐指令增量；组从 `GETSTATIC F`（+1）起，到
`IASTORE`（-3：数组引用 + 下标 + 值出栈）归零收尾。嵌套 `GETSTATIC F`
（值链里再读同一数组——grunt 用前一元素算下一元素）深度 +1，允许嵌套。

## 复核纪律

rewrite 对每个字段**重新收集跨度再擦**：同 clinit 里多个种子字段时，先
擦的字段改变布局，后擦的字段必须按新 DOM 重推。逐字段全有或全无：任一
跨度擦除失败即放弃该字段（已擦的跨度语义仍安全——普查门保证无类外读，
残余读检查兜底）。

## 残余读检查

擦完逐条扫 clinit：该字段的 `GETSTATIC` 必须清零，否则视为形状误判，放
弃该字段（防收集器把非种子读吸进跨度）。
