# string_cache_fold：字符串缓存解密结果回填

## 形态

Kasumi 的字符串加密（Blowfish，次要面）把密文存进静态字段，运行时经
`javax.crypto` 解密。密文载体与调用点有两种形态：

```java
// 形态 1 读缓存（<clinit> 已把明文解好缓存进 String[]）：
getstatic owner.a : [Ljava/lang/String;
iconst_1                      // <int idx>
aaload                        // → ldc <明文>

// 形态 2 直接解密（密文 byte[][] 在场，调用点现场解密）：
getstatic owner.b : [[B
iconst_1                      // <int idx>：密文行下标
aaload
dup
arraylength                   // 实参 len = 行长
iconst_0                      // <int off>：行内偏移（常量）
invokestatic owner.a ([BII)Ljava/lang/String;   // → ldc <明文>
```

形态 2 的解密器 `a(bytes, len, off)` 内部是反射
`Cipher.getInstance("Blowfish/ECB/PKCS5Padding")`（密钥 16 字节硬编码，由
CFF dispatcher 填充）；它消费的正是本 getstatic 装载的那一行密文——栈形
保证，与 owner/name 无关。

## 为什么可以静态还原

两种形态的调用点都是**纯数据拉取**：形态 1 从缓存数组按下标取一条明文，
形态 2 取一行密文现场解密。解密结果在运行期是确定的（密钥与密文都是编译
期事实），但复现 Blowfish（CFF 模拟提 key + 密码学实现）工程量大且与调用
点改写正交——树版与本仓都走**旁证路线**：用 `KasumiStringDumper`（加载原
jar、触发 `<clinit>`、反射读缓存/调解密器）把「(owner, 字段, 下标) → 明文」
导出成 TSV，改写时按表回填 `ldc <明文>`。

也就是说：**本残留的明文来自旁证，不来自解密器复现**。形状判据只负责找
站点；「这个下标的明文是什么」完全由 TSV 回答。无旁证不改写——这不是保
守性选择而是数据流事实：明文本身就是要回填的内容。树版对空映射的处理是
整个 pass 静默跳过，语义相同。

## 关键事实（判据的地基）

- **getstatic 描述符是结构性钉面**：`[Ljava/lang/String;`（形态 1）与
  `[[B`（形态 2）——普通程序也有 `getstatic String[]; idx; aaload` 形状
  （正常数组读），所以形状判据只是选面，定案靠明文表命中。
- **跨度是前缀钉面**：形态 1 钉到 aaload、形态 2 钉到 invokestatic 为止，
  后继指令与本站无关（树版 `nextRealAfter` 同口径）。
- **解密方法名不钉**：形态 2 的表键用 `byte:` 前缀 + getstatic 字段名
  （dumper 口径），方法名样本相关；栈形已锁定数据流。
- **TSV 是换代表面**：四列 `owner \t key \t index \t plaintext`，owner 点
  分；plaintext 转义 `\\ \t \n \r`。格式钉自 dumper 实现，内容随样本走，
  按 S3 对待——Era 分支是它唯一合法的落点。
- **明文落池要走 modified UTF-8**：classfile Utf8 没有 4 字节形（增补平面
  拆代理对、NUL 走 `C0 80`），直接把 UTF-8 字节塞进池会产生 JVM 拒收或
  误读的条目。

## 与相邻形态的区别

- **indy_decode（Kasumi indy 面）**：那个还原调用点**目标**（解密 bsmArgs
  求出 owner/name/desc），本残留还原**字符串字面量**（回填解密结果）——
  两者作用在同一混淆器的不同残留线，可共存于同一样本（SnakeGame 上
  indy_decode 管 339 个 indy，本残留管 170 个缓存站点）。
- **number_fold（数值藏形跨度）**：跨度折叠的改写原语同源（锚点原地换 +
  其余擦除），但那边的值由求值就地算出，本残留的值来自外部旁证表。
- **Booger 家族的字符串解密**（`ConstantCall` 折叠）：那边解密体是纯算术
  静态方法，能被常量折叠就地消解；Kasumi 的解密走 `javax.crypto` 反射，
  无法在静态求值子集内跑——所以走旁证而不是模拟。
