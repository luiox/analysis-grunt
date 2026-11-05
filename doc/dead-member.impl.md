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

## 普查漏计修复（Kasumi 批，全容器口径）

普查曾是「压缩成功类才作引用方」：`compactFailed` 类被整体排除，其对外引用
从活引用集蒸发——Kasumi 专链产物 16/25 类带非零长 Raw 属性压池被封闭集拒
绝，调用方 `w` 池里 `t.aa (III)I` Methodref 缺席，`t` 类 93 方法全删、运行
期 NoSuchMethodError（取证全文与修复后数字见
[kasumi-verify.md](kasumi-verify.md) §3；通用链单跑同病，`ref-rejected` 0 →
28）。修后普查分层：会话可开 → 枚举池；会话打不开 / 普查异常 → 原始字节骨
架解析（`readClass`，只解头/池/成员目录，Raw 面不挡路）再枚举；骨架也解不
开 → census-blind，本轮成员删除与空壳回收整体放弃。压缩失败的类仍不作删除
候选（字节面封闭集之外，两边宁留），但引用照入普查；普查挂起时间片跑全量
（proof 类数门同门）。同病同修：`field_census`（ConstValuePromote /
SeedStrip 的访问面普查）补 `Stat` 完整性台账、不完整整轮放弃；
`booger_anno_strip` 引用门会话打不开的类按引用在册关死。

## 伪 bridge 豁免核查（本批取证结论，不改门）

核查诉求：「ACC_BRIDGE 零引用豁免会误保伪 bridge 死代理，是否需要非真桥
排除面（如带操作数 xload 的伪 bridge）」。取证结论：**不改**。

- ACC_BRIDGE 在 Grunteon 3.0.0 生成面不携带信息：FakeSyntheticBridge（跑在
  MethodRenamer 之后）给全体非 abstract 方法补 ACC_BRIDGE|ACC_SYNTHETIC——
  真方法、indy 目标、死代理同一形状。「真桥排除面」（实例 + 同名转发体）
  会把这类罐里几乎所有成员从豁免里摘出去，恰好打掉豁免要保的对象。
- 豁免的触发条件是 bsmUnresolved（容器存在未收敛 BSM）：该前提下任何
  public synthetic/bridge 成员都无法证明零引用死（BSM 运行时 findStatic
  的目标没有 CP 明文引用），误保死代理是设计内的保守价。indy_fold 收敛后
  （bsmFailed=0）豁免不再触发，死代理走 gateB + 引用普查正常删除；链序上
  indy_fold 恒在 dead_member 之前。
- 「带操作数 xload 的伪 bridge」体形排除对 indy 目标同样成立不了：indy
  目标是真方法体，普遍带操作数装载，与死代理形状无法区分；pureArith
  （gateA）已是唯一不进豁免的体形删除面。

误保窗口只在 bsmUnresolved 轮打开，不构成正确性缺陷；ProxyInline /
FieldAccessProxy 落地后死代理变多，同窗口内同价保留。

## TEST-obf「2 空类」取证（budget-argv 批，结论：不复现，门不动）

上批交接称 TEST-obf 输出残留 2 个空类（`<init>`-only 门没接住）。本轮逐
成员取证三面核对，**全部为零**，原观察在现存产物上不可复现：

| 面取的样 | 空壳形态类数 |
|---|---|
| TEST-obf 输入罐（210 类） | 0 |
| 上批 base 链输出（63c4f33 快照） | 0 |
| 上批 cur 链输出（ce8b4a7） | 0 |
| 本轮 fix/budget-argv 全链输出 | 0 |

「空壳形态」口径：fields 空 ∧ methods 只剩 `<init>`（含参算子一并算，
含全零成员类）。TEST-obf 上 DeadMember 的空壳摘除门零触发（无候选、
无摘除行）——上批 2f432e0 落的「`<init>`-only + 零字段 + 全容器引用
普查 + main 门」已把该罐吃干净。**不新增「仅 public 无参构造 + 零字段」
放行形态**：没有活样本支撑，放行只会在无证据面扩大摘除权。若后续语料
再现此形态，先补罐再议。
