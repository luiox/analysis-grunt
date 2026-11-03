# number_fold：藏形数值常量跨度

## 形态

Grunt 换代后不再把字面数值直接写成常量装载，而是把值藏进一条**等价运算跨
度**：跨度由纯常量装载与封闭运算构成，字节码里看不到立即数，只有算完才知
道值。已见的生成面有五式：

int 值（两条走廊都是纯噪声——符号扩展保低 32 位，XOR 按位运算，截断回 i32
与 plain 异或等价）：

```text
  <i32 a>  i2l  <i32 b>  i2l  lxor  l2i      ; a ^ b
  <i64 a>  l2i  <i32 b>  ixor                ; (int)a ^ b
```

long 值（键串基 32 编码，运行期经 `Long.parseUnsignedLong` 还原再异或）：

```text
  ldc "key"                                  ; Long.toString(key, 32)
  <i32 32>
  invokestatic Long.parseUnsignedLong(String,I)J
  <i64 obf>
  lxor                                       ; key ^ obf
```

浮点值在整型跨度外面再包一层 JDK 位形换算（跨度产出的是 IEEE 位形）：

```text
  <int 跨度>  invokestatic Float.intBitsToFloat(I)F     ; → f32
  <long 跨度> invokestatic Double.longBitsToDouble(J)D  ; → f64
```

## 判据（S2）

跨度运算封闭、无栈外输入：所有操作数都是紧邻的纯常量装载，运算 op 固定
（I2L/LXOR/L2I 走廊、L2I/IXOR、parseUnsignedLong+LXOR），包装调用的
owner/name/desc 与 JDK 语义确定。混淆器无法在保持功能的前提下改变跨度的
产出值——这是「跨度求值即常量」的结构事实。跨版本的敏感面在**形状**：操
作走廊或键编码方式换代可换（S2 需复核，不是 S1）。

## 与相邻形态的边界

- `constant_call` 折的是「容器内纯算术方法」的调用点；本残留折的是**方法
  体内的内联跨度**，被调面只有 JDK 位形换算（`intBitsToFloat` /
  `longBitsToDouble` / `parseUnsignedLong`），不涉及容器内方法。
- `const_args::collectArgs` 只认纯常量装载——藏形跨度夹在分支/调用实参区
  里时下游全部失明；本残留排在实参收集面之前，是它们的解锁前置。

## 改写

跨度锚点（首条装载）原地改成结果常量装载（保 `srcPc`），其余跨度指令擦
除。不新增合并点、不插跳转。
