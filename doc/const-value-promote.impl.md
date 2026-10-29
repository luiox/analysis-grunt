# const_value_promote 实现笔记

## 数据流

`Site{uri, clinitIndex, fieldName, fieldDesc, putSrcPc, ValueLoad}` →
`DirtySet{uris, promoted, attempted, failed}`。

## 值面解码

`const_args::decodeValueLoad`（工具箱）：与栈值面 `decodePush` 是两个事实
——接受 ldc String，拒绝 aconst_null。返回 `ValueLoad{kind, bits, str,
slots}`，ev::Value 装不下字符串所以独立成结构。

## 值的三个通道

- `internInteger`（i32，会话侧 intern）
- `cp.addLong/addFloatBits/addDoubleBits`（u2 域检查，LDC2_W 是 CP2）
- `addString(addUtf8(...))`

## 复核纪律

rewrite 按 `uri + 字段名 + 描述` 重定位字段、按 `putSrcPc + memberOf` 重定
位写入点，**值重新解码并与 ScanResult 携带值按位比对**（kind + bits +
str）。scan 之后同方法其他字段可能已改写，不盲改。

## 台账

`promoted` 计数字段数；trace 面输出 `promoted/attempted/failed`。
