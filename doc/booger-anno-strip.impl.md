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

## 注解码码深度守卫接线（mjbase 0.0.18 随升批，已接线）

mjbase #158 回执：`oo.class` 崩因是 libjbc 注解码码互递归栈耗尽，守卫
（`annotation_depth`）落在 member_remapper 的注解族解码收口。本仓 grunt
侧有**直调 libjbc `decodeAttr` 的路径**，不在该守卫面内，逐位盘点：

| 直调位 | 解码对象 |
|---|---|
| `booger_anno_strip.cpp` stripTable / countIn | 类/方法/字段属性槽（注解族） |
| `constant_call.cpp` dumpPcRefs（Code + 嵌套） | 诊断面属性槽 |
| `dead_member.cpp` annotationStrings | 注解族属性槽普查 |
| `indy_fold.cpp` bsmEntries | BootstrapMethods（守卫恒放行，形状归一） |

mjbase 0.0.18 入包后（`mjbase/util/annotation_depth.hpp` 伞头可用）上述
直调位已按「注解族解码前查深度」接线（`mjbase::util::attrSafeToDecode`，
预检拒绝按「解不开原样保真」fail-open 跳过），调用点形状不变。

**负例测试咬出第 7 面：cp_compact**。`jbc::ir::cp_compact` 会对全部属性槽
（类/字段/方法 + Code 嵌套）`decodeAttr` 重映射注解内 CP 引用（jbc
cp_compact.cpp `planAttr`），敌意深嵌注解不走 Result 错误通道、直接栈溢出
（单测进程实证）。libjbc 上游挂账不动，grunt 侧在 `container.hpp` 加
`classAnnoDepthSafe`（同一批槽位过 `attrSafeToDecode`，Code 槽借道下探嵌
套属性），compact 前置到三处调用点：`dead_member.cpp` 首轮压缩与二次压缩、
`booger_anno_strip.cpp` 剥注解后压缩——越限类按既有 compact 失败通道
「原样保真」（CP 不重编号、字节不动，宁少带走死 Utf8）。

负例测试 `test/unittest/annotation_guard_test.cpp`（原始字节夹具，jasm 表
达不了千层嵌套）：5000 层 `'@'` 深嵌注解类过 BoogerAnnoStrip / DeadMember
不栈溢出、字节原样保真、不牵连同容器普通类清理；合法浅嵌 `@Booger` 照常
普查剥离（fail-open 回归）。
