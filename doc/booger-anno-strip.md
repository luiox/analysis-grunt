# booger_anno_strip：@Booger 运行时注解剥离

## 形态

混淆器在被保护类上挂运行时注解：

```java
@Booger(d2 = {...}, mv = 100)
public class Fx { ... }
```

注解**类名随机**，但描述符与参数键固定（d2 = 密钥表、mv = 轮数）——它是
字符串解密的运行时锚点（S1：decrypt 体读这个注解拿参数）。解密体被常量
折叠清空后，注解条目就是死数据。

## 判据

1. 注解类型描述符 == `Ldev/realme/Booger;`（家族知识
   `knowledge/booger_names.hpp`）；
2. 容器门：全容器 CP 无 `dev/realme/Booger` 的 Class 引用（解密体若还活
   着必有引用；注解类自身 thisClass 自引不算）。有引用即整站放弃——剥注
   解会让活着的解密体拿不到参数。

## 改写

类/方法/字段三张属性表里，从 RuntimeVisibleAnnotations /
RuntimeInvisibleAnnotations 槽内剔除该注解条目；槽空则整槽删除；落盘后压
缩一次带走死 Utf8。

## 台账

`annos` 计数剔除条目、`classes` 计数动过的类；容器门被拦时 trace 明示。
