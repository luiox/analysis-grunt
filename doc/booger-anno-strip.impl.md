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

## 注解码码深度守卫接线核查（budget-argv 批，结论：挂账）

mjbase #158 回执：`oo.class` 崩因是 libjbc 注解码码互递归栈耗尽，守卫
（`annotation_depth`）落在 member_remapper 的注解族解码收口。本仓 grunt
侧有**直调 libjbc `decodeAttr` 的路径**，不在该守卫面内，逐位盘点：

| 直调位 | 解码对象 |
|---|---|
| `booger_anno_strip.cpp:30,88` | 方法/字段属性槽（注解族） |
| `constant_call.cpp:258,267` | Code 属性 + 嵌套属性 |
| `dead_member.cpp:114` | 属性槽普查 |
| `indy_fold.cpp:65` | BootstrapMethods |

接线条件是 include `mjbase/util/annotation_depth.hpp`（自包含伞头）。
**mjbase 0.0.5 钉版包里没有该头**（`util/` 只有 pair/span/time_util/triple），
按红线不跨仓改、不动钉版——挂账等 mjbase 升级后接线。届时六处直调位
按「注解族解码前查深度」接线即可，调用点形状不变。
