.. _const-pool-restore:

const_pool_restore：v2 伴生池常量调用点回填
==============================================



形态
----

Grunt v2 ``ConstPoolEncrypt`` 把类内字面常量改写成对伴生池类静态字段的读取。罐 B 语料实测的伴生池形态：普通 public 类（非 final），super = java/lang/Object，全部字段 static int 且每个字段带 ``ConstantValue`` 属性，无 clinit，方法面只有一个返回 ``PROTECTED BY GRUNT KLASS MASTER`` 的水印方法（static ()Ljava/lang/String;，体两条 LDC 字符串 + ARETURN）。字符串伴生类是另一拨（数值字段走 clinit 算术写不带 CV，字符串字段走解密器三连写，水印方法文字不同：罐 B 实证 ``I AM WATERMARK`` / ``PROTECTED BY SPARTAN EVERETT``），钉形只认形状不认文字。生成源版本的形态描述（PUBLIC FINAL + clinit 逐字段解密回填）与罐面有出入，判据以罐为准：

::

     调用点：  GETSTATIC Companion.const_xxxxx : T     （原 ldc 常量位）
     伴生类：  public，super = java/lang/Object，字段全部 static 数值形
               且带 ConstantValue 属性，无 clinit，附水印方法
     字符串面  LDC 密文 + INVOKESTATIC 自家解密器
               (Ljava/lang/String;)Ljava/lang/String; + PUTSTATIC（clinit）

伴生类与解密器体在 heavy 配置下会被二次加密（HeavyEncrypt，默认关），本残留只消费解密链已折回的形态：数值面靠 ``ConstantValue`` 属性（static + 零写 = JVM 权威值），字符串面靠解密器模板钉形 + 逐码元自逆 XOR 直解。


判据
----

- 数值面逐字段判据（不设类级门）：static、非 volatile、描述符 ∈ {B,C,I,S,Z,J,F,D}、容器级写普查零写、普查有读、``ConstantValue`` 属性在且池条目 tag 与描述符相容。运行期值 = CV 属性值：罐 B 语料 LinkRead 实证 HotSpot 对非 final static 字段照样执行准备期 CV 赋值（3/3 CV-HONORED），GETSTATIC 换 LDC(CV) 语义保持。伴生池只是本面特例，javac 的 static final 常量族同面收走。
- 字符串面伴生类门：非 interface/abstract/enum、super = java/lang/Object（不钉 ACC_FINAL）、字段全部 static 且受援描述符、方法面 ⊆ {<clinit>, 24 条模板解密器, 水印方法（static ()Ljava/lang/String;，体两条 LDC 字符串 + ARETURN）}。
- 字符串字段：容器级写普查恰一次写且在伴生类 clinit、写点形状 = [LDC 密文, INVOKESTATIC 自家解密器, PUTSTATIC 本字段]、解密器体 24 条指令逐字钉（StringBuilder 逐字符 charAt ^ key 链，key 是体内唯一自由变量）、密文逐码元 XOR key 直解（S1 算术自证）。
- 调用点：GETSTATIC 解析成员命中已登记字段。

写普查走 field_census（promote 后指令面），普查不完整整轮放弃（宁误留不误删，与 const_value_promote 同一口径）。唯一豁免是验证器判死：promote 以 BadInstructions 拒绝的方法（指令流带未定义操作码、悬垂分支、异常表越界）所在类在标准 JVM 链接期必被 VerifyError 拒掉，方法体永不执行，按死亡不计入完整性。罐 B 语料上 14 个类的 16 个方法属此类（LinkProbe 逐类强制链接 14/14 VerifyError），不豁免则本 pass 在该语料上永远整轮放弃。


改写
----

按 ``srcPc`` 重定位 GETSTATIC（op 复核）后成员复核（owner/name/desc 与站点一致才动），单条指令原地换常量装载（保 ``srcPc`` ；数值走 makeConstLoad 短形态优先，字符串 intern 进池走 LDC/LDC_W）。无擦除、栈形逐位等（long/double 双槽两读同形），帧链走 mutateMethodWithFrames 正统写回，前置 roundTripSafe。

链序：数值面与 ConstValuePromote 分工（本面收 ConstantValue 属性族，ConstValuePromote 收 clinit 写一次无 CV 族，互不重叠），放 ``grunt.NumberFold`` + ``grunt.ConstValuePromote`` 之后跑；字符串面只依赖解密器体未被二次加密。本 pass 之后 ``grunt.ArithFold`` 吃回填出的常量对。


已知边界
--------

- HeavyEncrypt 配置（默认关）下伴生 clinit 与解密器体被 NumberEncrypt/StringEncrypt 二次加密，解密器模板钉形失效，逐类 fail-closed。
- 数值面消费 ``ConstantValue`` 属性：JVMS 只规范 static final 字段的 CV 赋值，HotSpot 对非 final static 也执行（罐 B 语料 3/3 实证）；其他 JVM 实现若不执行，回填值与运行期值可能不符，如实登记。
- 写面靠指令普查兜底，反射 / JNI 写不可见（与 const_value_promote 同一信息边界）。
- 帧重算失败的方法整方法回滚（罐 B 语料 20144/938982 站点，集中在混淆器产出的巨型陷阱方法，其 StackMapTable 本来就超出帧重建能力）：站点保留 GETSTATIC 原样。本 pass 无擦除，字段不删，运行期照常读到 CV 值，语义不变，只是该 2% 站点未还原。
- 伴生类本体不删：字段回填后成为零读死重，交 DeadMember 与垃圾类删除面（缺口 #14）。
- 字符串回填是 ldc 字面量（驻留池），伴生字段原值是 clinit 期新串：内容相等、实例身份不同，与 string_pool_fold 的字符串池回填同一口径，如实登记。

判据源锚与实现细节见 ``src/grunt/const_pool_restore.hpp`` / ``const_pool_restore.cpp`` 文件头注释。
