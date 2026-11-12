.. _field-access-proxy:

field_access_proxy（字段访问代理）
====================================================


混淆器把字段访问点包一层 static 代理方法：调用点改成 ``invokestatic proxy(...)`` ，代理体只是实参过手 + 单次字段访问 + 按字段类型返回。字节码里原本的 ``getstatic/putstatic/getfield/putfield`` 引用从此消失。


生成面（Grunteon 3.0.0，FieldAccessProxy 实跑取证）
----------------------------------------------------------------------------------


Grunteon 的 ``FieldAccessProxy`` transformer（Category.Redirect，与 InvokeProxy同族）对每个可解析的字段访问点（chance 概率）造一个 ``public static`` 代理，调用点原地换成 ``invokestatic`` ：

.. list-table::
   :header-rows: 1

   * - 访问点
     - 代理描述符
     - 代理体
   * - ``GETSTATIC Owner.f : T``
     - ``()T``
     - ``getstatic`` + 匹配返回
   * - ``PUTSTATIC Owner.f : T``
     - ``(T)V``
     - 值装载（槽 0）+ ``putstatic`` + ``return``
   * - ``GETFIELD Owner.f : T``
     - ``(Lowner;)T``
     - ``aload_0`` + ``getfield`` + 匹配返回
   * - ``PUTFIELD Owner.f : T``
     - ``(Lowner;T)V``
     - ``aload_0`` + 值装载（槽 1）+ ``putfield`` + ``return``


实测形状（javap， ``outer=true`` ）：

::

   public static int get_sInt_TLm11();          // static getter
     0: getstatic  #14  // Field FieldProbe.sInt:I
     3: ireturn

   public static void set_iLong_RpVja(FieldProbe, long);   // instance setter
     0: aload_0
     1: lload_1
     2: putfield   #43  // Field FieldProbe.iLong:J
     5: return

   // 调用点：invokestatic FieldProbe$FieldProxy.get_sInt_TLm11:()I


- 代理落点：调用者类内（ ``public static`` ）或调用者类的生成外置类 ``C$FieldProxy`` （owner 与字段都 public 时）。
- 字段 owner 可以在容器外（ ``System.out`` 也被代理）。
- 方法名 ``get_/set_<字段名>_<随机 5 字符>`` ；后续 MethodRenamer 会整体改名， ``FakeSyntheticBridge`` 会给全体方法补 ACC_BRIDGE|ACC_SYNTHETIC， **名字、标志、家族名在产物里全部不可依赖** 。


判据要点
--------


只钉方法体形状 + 引用普查 + 初始化暴露检查（与 ``proxy_inline`` 同口径）：

1. 体形：static、有方法体、非初始化器、非 synchronized；直线体（无标签、无异常表）= 每个形参槽一条匹配类型的 ``xload n`` （long/double 占两槽）+单条字段访问 + 匹配返回；字段与描述符核对（描述符必须恰为按该字段该有的形状，见上表）。
2. 引用普查：只对容器内有 ``invokestatic`` 调用点的代理动手；零引用代理是死码，归 DeadMember。
3. 初始化暴露检查：调用点越出代理 owner 类时，owner ``<clinit>`` 必须中性（ ``scan_tools::classInitNeutral`` ，与 constant_call / proxy_inline 单源）。字段 owner 的初始化时机不受改写影响：代理体内的字段访问本来就触发它。


改写
----


调用点原地换 op + 换字段引用（保 ``srcPc`` ，栈效应不变，栈帧无需修补）；代理本体不动，改写后变零引用死码交下游 DeadMember/DeadCode。


边界
----


- 真桥方法（javac 生成）是实例方法、同名单参适配转发，与字段代理形状不相交；带体非直线的伪桥由体形检查拒绝。
- ``PUTSTATIC``/``PUTFIELD`` 改回时若字段 owner ``<clinit>`` 非中性也不加检查：混淆产物里代理体内的字段访问同样触发 owner 初始化，首次触达顺序不变；唯一变化的是代理 owner 的初始化暴露，已由第 3 条检查兜住。
- 套娃（代理的代理）不构成本残留站点：代理体内是字段访问，不是 ``invokestatic`` 。
