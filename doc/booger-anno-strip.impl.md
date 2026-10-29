# booger_anno_strip 实现笔记

## 数据流

`Site{uri, annoCount}` → `DirtySet{uris, annos, classes}`。容器门
（`gateBlocked`）在 scan 判定，rewrite 开头再查一次同一布尔——scan 与
rewrite 之间容器不变（三段同进程顺序调用），门结果直接复用。

## 属性槽操作

`decodeAttr` 得 `AnnotationsAttr`，按 `anno.typeCp` 的 utf8 比对描述符；
保留列表非空走 `AttrSlot::mutate()` 置 Modified，为空把整槽从属性表删除
（倒序 erase 防止下标漂移）。

## 挂账

注解类本体（dev/realme/Booger 类文件）的删除属死类删除，挂账于
ResourceContainer 缺 removeResource（见 dead-member.md 台账节）。
