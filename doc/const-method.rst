.. _const-method:

const_method：单常量方法
====================================



形态
----


混淆器把取常量的表达式包一层无参静态方法，调用点与方法体一一对应：

::

   public static /* bridge */ /* synthetic */ Class t() {
       return Long.TYPE;                       // getstatic Long.TYPE ; areturn
   }
   public static int key() { return 0x5E3A1C42; }   // ldc ; ireturn


方法体没有分支、没有实参、返回值与运行环境无关，每个调用点静态等价于一条常量装载。典型规模：一个中型样本里 ``getstatic + areturn`` 形态数百处，多数是原始类型 ``TYPE`` 取值器（ ``Long.TYPE`` / ``Integer.TYPE`` / …）。


为什么 TYPE 可以内联
--------------------------


``getstatic java/lang/Long.TYPE`` 的取值是 JVM 规范定义的编译期常量引用：JVMS §4.4.1 允许 CONSTANT_Class 条目引用原始类型描述符名（ ``J`` 、 ``I`` 、 ``Z`` …），所以调用点可以落成 ``ldc (Class "J")`` 。 ``Boolean.TRUE`` 这类 **堆对象常量** 没有可静态编码的等价形式，内联到调用点需要复制对象身份，不可行。


与相邻形态的区别
----------------


- **constant_call** （有实参折叠）：实参全常量 + 方法体纯算术，靠求值器跑出值。const_method 是它的零实参特例中求值器覆盖不到的部分（getstatic不在求值子集内），两者互不重叠。
- **字段池化** （getstatic 读字段池）：读取点折叠依赖字段唯一写 + 常量值的容器级证明，是独立残留；本残留只处理读的是 JDK 规范常量这一特例。
- 三个形态都只产一条常量，下游 ``== 常量`` / switch 的死支消解都交给mjbase 的常量折叠链。
