.. _indy-decode:

indy_decode：Kasumi 全量 indy 化调用点
==============================================================



形态
----


Kasumi 混淆器把 **全部** 成员访问改写成 ``invokedynamic`` ：static/virtual 方法调用、字段读写、构造器调用一个不剩，每类注入一个 8 参 bootstrap 与一个XOR 解密器。调用点上不再有任何目标明文，目标编码在 ``BootstrapMethods`` 静态参数表里，且参数本身也是密文：

::

   // 调用点（name/type 只是摆设）：
   invokedynamic bsm (I)I { bsm, [0, "密文owner", "密文名", "密文desc", 0] }
   // BSM（每类一个，随机名，8 参指纹）：
   //   (Lookup, String, MethodType, Integer, String, String, String, Integer) CallSite
   // 解密器（每类一个，(String;)String，体内藏 int key）：
   //   key = K1 ^ K2     ← 双层：charAt; LDC K1; LDC K2; IXOR; IXOR; i2c
   //   解密 = 每 UTF-16 码元 XOR key 低 16 位
   // 分派 = findStatic / findVirtual / findGetter / findSetter /
   //        findStaticGetter / findStaticSetter / findConstructor


静态参数表固定五元 ``[mode, encOwner, encName, encDesc, extra]`` ：owner 是点分形（ ``java.lang.Math`` ），name/desc 同为密文； ``(mode, extra)`` 二维分派7 形态， ``0,0`` static、 ``0,1`` virtual、 ``1,0..3`` getfield/putfield/getstatic/putstatic、 ``2`` ctor。ctor 的 encDesc 解出空串，init 描述符由调用点描述符的参数段合成，indy 返回类型恒为 ``Lowner;`` 。


为什么可以静态还原
------------------


key 是 **编译期事实** ：它在解密器方法体里，以两条 ``ldc`` 常量 + 两条 ``ixor`` 的固定形状在场；bsmArgs 挂在 ``BootstrapMethods`` 属性里；解密是逐码元XOR；分派表只有 7 行。整条链没有一步需要执行样本代码，把 key 提出来、把密文异或回去、按表落指令，得到的调用点与运行期 ``ConstantCallSite`` 解析结果结构等价。安全性由核对兜底：解密产物必须与 indy 调用点描述符吻合（static 全等、virtual 多一个 receiver 槽、字段按 4 路形状、ctor 返回 ``Lowner;`` ），不吻合即拒绝，混淆器要保功能就必须让两者吻合，改了自己就坏。


关键事实（判据的地基）
----------------------


- **8 参 BSM 描述符是结构性指纹** ： ``(Lookup, String, MethodType, Integer,String, String, String, Integer)CallSite`` ，通用 indy（LambdaMetafactory等）不落这个形。
- **解密器形状钉 key** ：定位体内 ``i2c`` 后按布局回溯两层 ``ixor`` ，取其前驱两条 int 常量异或；兼容单层 ``LDC K; IXOR`` 。
- **mode/extra 表是换代面** ：数值是混淆器实现选择，换代可能重排，只在Era 分支（家族判明为 Kasumi）内使用。
- **密文走 MUTF-8 码元面** ：Java 字符串合法含 lone surrogate（类文件里是3 字节 CESU-8 形），严格 UTF-8 解码会拒真，解密按 UTF-16 码元异或，产物按 Java 规则重编码。


与相邻形态的区别
----------------


- **indy_fold（通用 indy 面）** ：那个靠 libjsim 模拟执行 BSM 求目标，面向BSM 是可模拟的纯函数的通用形态；本残留 key 链全静态，面向 Kasumi专形（8 参 BSM + 解密器）， ``probeEra`` 判家后才进。同一 jar 上两者不争站点：Kasumi BSM 的 8 参签名 + find* 分派不在 indy_fold 的纯目标子集内。
- **string_cache_fold（字符串缓存面）** ：那是字符串解密结果的缓存旁证，与调用点目标还原不同面，可共存于同一样本。
