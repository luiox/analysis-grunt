# dead_member 实现笔记

## scan/rewrite 的分工

scan 只产**形状候选**（方法体形状 / 字段访问位 / clinit 拆空）；零引用判
定必须发生在 rewrite——它依赖「先压缩再普查」这个改写动作，scan 不能改
容器。所以 rewrite 里候选可能被引用门拒掉（`rejectedByRef`），这是设计内
的，不是 bug。

## 压缩的双重身份

(1) 普查前提（死条目污染零引用判定）；(2) 删除后的收尾（被删成员的描述
符 utf8 从池里带走）。两步都落盘；仅压缩变小的类也进 `dirty.uris`（落过
盘就要过 proof 的可重解析门）。

## 纯算术形状词汇表

nop/const/locals（0x00-0x2d）、store（0x36-0x4e）、栈操作（0x57-0x5f）、
算术/转换/比较（0x60-0x98）、if/icmp/goto（0x99-0xa7）、return
（0xac-0xb1）；`ldc/ldc_w` 单独查 ConstSem 必须是数值类别。

## 引用键口径

方法 `owner::name::desc`，字段 `owner::name`。BootstrapMethods 引用的
MethodHandle→Methodref 在压缩闭包内存活，自然计入活引用——indy 引导方法
引用的成员不会被误删。
