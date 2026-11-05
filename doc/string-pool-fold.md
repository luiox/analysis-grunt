# string_pool_fold：字符串池读点折回明文

## 形态

Grunt v2（`StringEncryptTransformer`，grunt2@36c0eef）与 v3
（`StringArrayedEncrypt`，grunt3@e41e852）把字符串字面量搬进每类一个的
`String[]` 池，读取点统一成三元组：

```java
getstatic owner.<pool> : [Ljava/lang/String;
<int idx>                      // 紧邻常量下标
aaload                         // → ldc <明文>
```

池的供给链（同类内三件套，全部生成面直线码）：

```java
// <clinit> 分配前导四连：
<int poolLen>
anewarray java/lang/String
putstatic owner.<pool> : [Ljava/lang/String;
invokestatic owner.<arrayInit> ()V

// arrayInit 逐块存池（v2/v3 只差密文形态一段）：
getstatic owner.<pool> : [Ljava/lang/String;
<int idx>
// ── v2 literal（Arrayed=false）：密文串 LDC + toCharArray ──
ldc "<密文>"
invokevirtual java/lang/String.toCharArray ()[C
// ── v3 arrayed（carray=true）：NEWARRAY T_CHAR + 逐码元 CASTORE ──
<int len>
newarray char                  // NEWARRAY T_CHAR
dup; <int e>; <int code>; castore   // × len
// ── 两形合流 ──
<long seed>                    // per-string seed（< 100000）
<int key>                      // per-string key
invokestatic owner.<dec> ([CJI)Ljava/lang/String;   // classKey 烧在体内
aastore
```

解密器 `([CJI)Ljava/lang/String;` 的算术核（v2 与 v3 **逐字同源**，两分支
`encrypt()` 的 Kotlin 全文一致；`createDecryptMethod` 的槽位编排也一致——
0=[C, 1-2=J(seed), 3=I(key), 4=state, 5=counter）：

```kotlin
// grunt2 StringEncryptTransformer.kt L389-403 == grunt3 StringArrayedEncrypt.kt L405-419
var n = key; var l = seed; var n2 = classKey xor n
for (i in cArray.indices) {
    n2 = n2 xor l.toInt() xor i.inv()
    n2 = n2 xor (n - i * cArray.size)
    n2 = (-n2 * n) or i
    cArray[i] = (cArray[i].code xor n2).toChar()
    val n3 = i and 0xFF
    n = (n shl n3) or (n ushr -n3)
    l = l xor n3.toLong()
}
```

状态只由 `classKey/key/seed/len` 决定、与密文无关，且逐码元 XOR——所以
`decrypt == encrypt`（自逆），明文可静态复算。

## 为什么可以静态还原

折叠一个读点需要回答「这个下标的明文是什么」。v2/v3 的供给链把答案全部
留在了类文件里：seed/key 是 arrayInit 块内的装载期常量，classKey 烧在解密
器体内，解密器是纯函数——**S1 级判据天然在场，旁证不需要**。这是与
string_cache_fold（Kasumi，Blowfish + CFF，走 TSV 旁证）的本质差异。

判据走结构自证：池链三件套逐字钉形（前导四连 → arrayInit 块序列 → 解密器
三窗口），钉形过了才解密、才折叠。任何一环缺席或被后续混淆打碎，整类
fail-closed 拒绝——residual 如实计，不猜。

形态判别按**常量 KIND**（arrayInit 块内密文位是 String 常量 → literal，
int 常量 → arrayed 长度）：v2/v3 的差异是密文形态**配置选择**
（`arrayed`/`carray` 开关），不是版本键——同一开关两个分支都在。Era 不
消费（缺口 #15 的 v2/v3 判据不在本批面内）。

## 关键事实（判据的地基）

- **解密器描述符是槽位钉面**：`([CJI)Ljava/lang/String;` 是 static 方法，
  槽位静态唯一——k=3、seed=1 不用猜；state=4/counter=5 由
  `createDecryptMethod` 的 ISTORE(4)/ISTORE(5) 编排钉死，核窗口的 52 条
  逐字序列（L4~L11 块，`sipush 255` 归一为 `ldc 255` 后仍由常量值钉住）
  是解密器「纯函数未被改写」的证明。
- **池读三元组是跨类形状**：普通程序也有 `getstatic String[]; idx; aaload`
  （正常数组读）——所以形状只是选面，定案靠整条池链 + 逐下标解密互证。
- **池字段/arrayInit/解密器不在本 pass 删除**：折叠后供给链仍被 clinit
  前导引用而存活（运行期空转填池，无读方）——整体摘除属压池面的后续
  批次，本 pass 只动读点。
- **明文落池要走 modified UTF-8**：与 string_cache_fold 同一面——classfile
  Utf8 没有 4 字节形；v3 密文码元可含代理区值（CASTORE 直存 UTF-16 码元），
  解出的明文含增补平面时按代理对重组编码。
- **蹦床一跳**：Grunteon 全链产出（boar 语料实证）会把 clinit 的
  arrayInit 调用藏进自家 indy 机器（ReferenceObfuscate：`invokestatic
  <helper> ()V` → helper 体 `[invokedynamic; return]`）。本 pass 只跟
  **纯结构**的直线静态蹦床（体恰 `INVOKESTATIC <self> ()V + RETURN`，深度
  封 2）——上游 indy 还原（IndyDecode/IndyFold）之后的产物形即可接通；
  indy 形态本身 fail-closed 报独立原因，不评 BSM。

## 与相邻形态的区别

- **string_cache_fold（Kasumi 字符串缓存）**：那边明文来自外部 TSV 旁证，
  形状判据只选面；本残留明文由池链静态复算（S1），无注入面。
- **indy_fold / indy_decode（indy 还原）**：那边还原**调用点目标**；本残留
  还原**字符串字面量**。两个面在池链上是上下游——上游把 indy 藏的
  arrayInit 调用还原出来，本 pass 的蹦床跟随才能接通真链。
- **number_fold（数值藏形）**：改写原语同源（锚点原地换 + 其余擦除），但
  那边的值由求值就地算出，本残留的值由池链解密得出。
- **era 判据（缺口 #15）**：v2/v3 无版本敏感判据（形态是配置不是键），
  本 pass 不消费 Era——判据缺口不在本批面内。
