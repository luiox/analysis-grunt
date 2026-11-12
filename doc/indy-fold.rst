.. _indy-fold:

indy_fold：invokedynamic 解密调用点
==========================================================



形态
----


混淆器把方法调用点改写成 ``invokedynamic`` ：字节码里不再出现目标方法引用，目标被编码进 bootstrap 方法（BSM）的静态参数表（通常是三段密文字符串 +一个形态开关），BSM 在首次链接时解密、拼装并查找出真正的目标句柄：

::

   // 调用点：
   invokedynamic #x  // name/type 只是摆设，真目标在 BSM 参数里
   // BSM（混淆器注入的 static 方法）：
   ConstantCallSite(Lookup.findStatic|findVirtual(
       Class.forName(decrypt(a)), decrypt(b),
       MethodType.fromMethodDescriptorString(decrypt(c), loader)))


每个调用点因此都是 **明文目标，但字节码里一个明文都没有** ，密文挂在 ``BootstrapMethods`` 属性里，靠运行期解密。任何按字符串常量 / 方法引用做匹配的规则在这种形态面前全部失效。


为什么可以还原
--------------


``invokedynamic`` 在链接期只解析出 **一个** ``MethodHandle`` （ ``ConstantCallSite`` 形态下终生不变），之后每次执行就是一次普通 invoke。所以只要能算出 BSM返回的句柄，调用点就静态等价于一条 ``invokestatic`` / ``invokevirtual`` ：调用点描述符与句柄描述符相同时，弹栈实参与压栈返回值完全一致，换指令不改变任何可观测行为。


关键事实（判据的地基）
----------------------


- **目标是算出来的，不是猜出来的** ：BSM 是类里的普通 static 方法，输入只有静态参数；在常量实参下模拟执行它，读出返回的 ``CallSite`` 即得目标。
- **reference_kind 决定落点指令** （JVMS 4.4.8）： ``invokeStatic`` (6) /``invokeVirtual`` (5) 换成对应指令即可； ``Special`` (7)/``Interface`` (9) 的栈语义不同，不换。
- **``asType`` 适配是语义边界** ：BSM 常对句柄做 ``asType(调用点类型)`` ；只有句柄描述符与调用点描述符一致（未适配）时换指令才等价。


与相邻形态的区别
----------------


- **字符串解密（ConstantCall 面）** ：那是调解密方法拿明文，调用点目标本来就在字节码里；本残留是调用点目标整个消失，两者可同时存在于一个样本，字符串解密链先跑，BSM 引用的解密方法才可能被折叠进常量。
- **通用 indy（LambdaMetafactory 等）** ： ``LambdaMetafactory`` 的 BSM 产出调用点类型实现的工厂句柄，不是直接指向目标的句柄，且强依赖运行期 ``Lookup`` 实参，不在本残留面内，模拟不通过自然拒绝。
