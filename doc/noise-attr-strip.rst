.. _noise-attr-strip:

noise_attr_strip：噪声属性摘除（massive Signature + SourceDebugExtension）
================================================================================

Grunteon v3 两类噪声属性（obf-behaviors 缺口 #13 的属性面）：

- **DecompilerCrasher 的 massive Signature**：类/方法/字段挂内容非法的
  ``Signature`` 属性（空串、空白块、缺 ``;`` 的类型串）。JVMS 4.7.9.1 的
  签名文法是上下文有关文法，非法串让 CFR 0.152 的签名解析器在类载入即抛
  ``ConfusedCFRException: Prototype is invalid`` ——整个类反编译不出来
  （t3_full 152/152 类全带，逐类扫描 0/152 实证）。JVM 对 Signature 只做
  反射 generics 视图消费（\ ``getGenericType`` 对非法串本就抛
  ``GenericSignatureFormatError``），字节码执行从不读它，摘除不碰行为。
- **AntiLLM 的 SourceDebugExtension**：JVMS 4.7.11 纯字节串调试属性，体
  是反分析文本（模拟 AI 摘要器的注入指令与策略噪声）。仅调试器消费，运行
  时零语义。


判据
----

``Signature`` 按**槽位形**严格过 JVMS 4.7.9.1 文法（类槽 ClassSignature、
方法槽 MethodSignature、字段槽 FieldSignature）：文法过 = 合法保留
（jsoup 真泛型 ``Ljava/util/List<Lorg/jsoup/nodes/Element;>;`` 不伤），
文法不过 = Crasher 串摘除。``SourceDebugExtension`` 整属性无条件摘除。

保守面：Signature 文法合法的一律不动（哪怕 owner 是混淆器改名的类）；
其余属性不碰。摘除后 cp_compact 带走死 Utf8（Crasher 串动辄数百 KB）。


改写
----

倒序 erase 属性条目 + CP 压缩；长度/偏移面随属性表收缩由 Attr 表语义自
带，无帧/字节码搅动。scan 定死站点，rewrite 只删。


验证
----

jsoup v3_full 实测（build13 主线 trace）：首轮 ``sig-invalid=31047
sde=323 → 846 类 31370 条属性摘除，proof=ok``；第二轮起 scan 归零（不
动点吸收）。深查批普查口径：junk Signature 残留 2→0。

unittest 4 测试：非法 Signature 按槽位形摘除、合法泛型保真、
SourceDebugExtension 摘除、干净类零搅动。
