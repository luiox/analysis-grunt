# proxy_inline：代理方法

## 形态

混淆器把方法调用点包一层 static 代理：调用点改成 `invokestatic proxy(...)`，
代理方法体只是把实参**逐槽过手**（每个实参槽一条 `xload n`）、**转发一次**
真实 invoke、按返回类型返回：

```java
// 调用点（virtual 目标的接收者被降成第一个实参）：
invokestatic C.add_redirected_x (Lrecv;II)I
// 代理本体（private static，落在调用者类内）：
aload_0
iload_1
iload_2
invokevirtual recv.add (II)I
ireturn
```

生成面的形状事实（InvokeProxy 一族）：

- static 目标：代理描述符 == 目标描述符，体 = 实参过手 + `invokestatic` +
  匹配返回；virtual 目标：代理描述符首参是接收者（`L目标owner;`），体里是
  `invokevirtual`，调用点同步降成 invokestatic。interface / special 目标
  **不生成**代理。
- 代理落在调用者类内（`private static`）或生成的外置类 `C$InvokeProxy`
  （`public static`，owner+method 都 public 时）；成员上带伪
  `bridge`/`synthetic` 标志与方法名无关的随机后缀——判据**不认名字、
  不认标志、不认家族**，只认体形。
- proxy 可能套 proxy（代理的转发目标也是代理）：单轮内联一跳，靠整链
  重复到站点归零。

## 为什么可以内联

调用点与真实目标之间的代理是**恒等转发**：弹栈实参集合与压栈返回值完全
一致，方法引用是链接期常量。把调用点的 `invokestatic proxy` 原地改成真实
目标的 `invokestatic`/`invokevirtual`，栈效应、初始化时序（见下）、异常
语义全部不变；代理本体随后变成零引用死码，交给下游 DeadMember/DeadCode。

## 初始化时序约束（判据的保守面）

`invokestatic proxy(C)` 是 C 的主动使用；改指真实目标后这层触发消失。
生成面的布置恰好保证这无副作用：调用点全在代理 owner 类自身（外置类是
新生成的空类，无 `<clinit>`）——执行调用点时 owner 要么已初始化、要么
初始化是空操作。判据把这写成可检查的事实：**调用点越出 owner 类的代理，
owner `<clinit>` 必须中性**（缺失或只剩 NOP/RETURN），否则整代理放弃。

## 与相邻形态的区别

- **const_method**（单常量方法）：转发的是**常量生产**，调用点落成常量
  装载；proxy_inline 转发的是**真实调用**，调用点落成 invoke。两者判据
  同构（体形 + 保守门），产物不同、互不重叠。
- **constant_call**（常量实参折叠）：对纯算术目标求值取值。代理体不是
  求值面（有转发 invoke），但被代理的真实目标若有常量实参，
  `ProxyInline → ConstantCall` 链序下两跳合并成一条常量。
- **field 访问代理**（getfield/putfield 过手体）：转发的是字段访问而非
  invoke，形状与内联方式都不同，是独立残留面。
