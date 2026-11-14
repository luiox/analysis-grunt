.. _coverage:

还原覆盖现状：能处理什么，不能处理什么
==========================================

本文回答两个问题：现在的工具能还原哪些混淆行为，还有哪些没做。逐个行为的产出结构、判据与源码锚在 :ref:`obf-behaviors` （缺口清单也在那里，编号 #1 到 #18），真实样本上的验证数字在 :ref:`corpus-ledger`。本文分四部分：已还原、无解、待办、边界。

结论先行：v2 / v3 两代混淆器的高频面与中频面都已还原并在真实样本整链验证；剩下的主体是垃圾类删除、噪声清理、MethodExtractor、ParameterObfuscate 四个待办面，加上几个信息边界（native 管线、运行期随机、改名）属于静态面上无解，不是工具欠账。


1 能还原的（已实现，真实样本整链验证过）
------------------------------------------

括号内是承载的 pass 名。每一条都在真实混淆样本上整链跑过（v2 样本 10486 类、v3 样本 3074 类，产物逐条目比对一致，JVM 校验失败集与输入相同），数字登记在 :ref:`corpus-ledger`。

**常量与数值面**

- 藏起来的数值常量：XOR 走廊、基 32 long 跨度、位形包装、v2 IAND/LAND 位拼模板（``grunt.NumberFold``）
- 搬进伴生类的字面常量：数值按字段 ``ConstantValue`` 属性读回，字符串按解密器模板直解，调用点换回常量装载（``grunt.ConstPoolRestore``）
- clinit 单点常量写：回填 ``ConstantValue`` 属性并摘写点（``grunt.ConstValuePromote``）
- clinit 种子建表跨度（``grunt.SeedStrip``）
- 绕圈子的算术：替换形状折回单条指令，纯常量算式直接算出结果；v2 算术替换、v2 跳转变异直线形、v3 算术替换三个来源一套实现（``grunt.ArithFold``）
- 字符串池读点折回 ``ldc``（``grunt.StringPoolFold``）

**调用点面**

- indy 调用点直接解密：v2 simple 形、v3 simple/heavy 形，材料通道 key 静态折回联动（``grunt.IndyRestore`` + ``grunt.MaterialKeyRestore``）
- 全常量实参的调用折叠成常量（``grunt.ConstantCall``）
- static 方法代理与字段代理调用点改回真实目标（``grunt.ProxyInline`` / ``grunt.FieldAccessProxy``）
- 常量方法内联（``grunt.ConstMethod``）
- v3 InvokeDispatcher 调用点反查回填（``grunt.DispatcherRestore``）

**控制流面**

- 跳到下一条的假跳转（``grunt.TrampolineSqueeze``）
- 常量可裁决的分支与 switch（``grunt.PredicateFold``）
- v3 控制流摊平反演（``grunt.CffUnflatten``）
- v3 异常桥还原（``grunt.ExcBridgeRestore``）
- v3 gate processor 折叠（``grunt.GateProcessorFold``）

**比较与清理面**

- hashCode 比较桥换回 ``equals`` / ``equalsIgnoreCase``（``grunt.StringEqualsRestore``）
- 死代码清理（``grunt.DeadCode``）、零引用成员删除（``grunt.DeadMember``）
- 伪 synthetic / bridge 标志（``grunt.FlagsStrip``）、残留 Booger 注解（``grunt.BoogerAnnoStrip``）

另有 Kasumi 专链三面（``IndyDecode`` / ``StringCacheFold`` / ``MetadataStrip``）端到端验收见 :ref:`kasumi-verify`。


2 不能还原的（静态面上无解，原因如下）
------------------------------------------

这些不是没写完，是 jar 里已经没有还原需要的信息，写明原因以免误报缺陷：

- **native 管线**：config 开关（boar 参考配置默认开），方法体随 native 库搬出 jar，类文件里只剩壳，参考 jar 上有 18461 个这类方法；``keyProcessorNativeCandidateRatio=1.0`` 一类配置还会把 CFF key processor 算术全部带走。没有配套 native 库，静态分析无从下手。
- **运行期随机检查**：ThreadLocalRandom 一类的值运行期才定，静态面永远不可裁决，永久排除。
- **改名还原**：类名 / 方法名 / 字段名被换成新名字，原名没有留在 jar 里任何位置，信息不可逆。要还原需要外部 mapping 文件作旁证，是外部工具职责，本仓不猜名字。
- **产物面破坏开关** （corruptCRC32 / corruptHeaders）与检测类面（HWID 校验调用）：不做还原处理。
- **SSA exp 摊平变体**：与已实现的摊平主形不同构（LookupSwitch + per-case stub），未实现，挂缺口 #4 余量。
- **v2 indy heavy 形**：v2/v3 的 heavy 形逐字同源，缺可靠的 Grunt2 版本判据，判据不明就不激活（缺口 #15），等语料积累。
- **HeavyEncrypt 配置** （默认关）：解密器与伴生 clinit 被二次加密，解密器模板钉失效，这种样本逐类跳过（fail-closed，宁误留不误删）。


3 还没做的（静态可解，还原面未实现，按价值排序）
--------------------------------------------------

- **垃圾类删除** （缺口 #14）：TrashClass / ClonedClass 生成的垃圾类，以及常量回填后变成零引用死重的伴生类本体（v2 样本默认配置不生成垃圾类，伴生类死重候选约 2700：2516 数值池类 + 154 字符串伴生类；垃圾类面要重产开开关的语料才有东西验证）。判据是零引用加生成面形状识别，删除需要全容器引用普查确认（反射 / 资源文本引用也要算进去）。
- **噪声与检测面清理** （缺口 #13）：两代水印字段与方法、Crasher / DecompilerCrasher 的 massive Signature 属性（还会让 javap 崩溃，取证全靠自写解析器）、AntiLLM 注入资源、HWID 校验调用摘除（删除需要 CFG 副作用证明）。
- **MethodExtractor 还原** （缺口 #9）：透传桩与伴生方法体合并、owner 类 accessor 内联、伴生类删除。面大价值中。
- **ParameterObfuscate 收窄** （缺口 #12）：私有方法描述符参数收窄 + 头部 checkcast 删除。
- **ConstBuilder switch 折回** （缺口 #6）：实现面已定义（switch 全表静态求值折回 ldc），但默认管线序把它饿死，现有语料上是 0 站点；要重产一个关 NumberEncrypt 或调序的 jar 才有东西验证。
- **v2 Controlflow 假调用残留与陷进环** （缺口 #16）：落空侧的垃圾调用点不是常量可裁决形态，TrappedSwitchCase / ChaosSwitch 的假 case 互跳闭包无专项还原面；思路是静态识别闭包与校验器式不可达后剪枝，交下游折叠与清死。常量回填落地后该面已解锁一大截（解锁链上死代码清理量 2964 到 688861 指令），本体还原面仍未实现。
- **NumberEncrypt arrayed 池** （缺口 #17）：long[] 池折回，池读形态与字符串池同构，要先折外层 classic 走廊；开关默认关，现有语料没有这个形态。
- **probeEra Grunt2 判据** （缺口 #15 的 v2 半边）：两代残留形状逐字同源无稳定差异，等语料积累出版本敏感的残留再补；v2 现场可处理的面不受影响（simple 形全结构自证，不依赖版本判断）。


4 已还原面带的边界（如实登记的限制）
------------------------------------------

- 数值回填消费 ``ConstantValue`` 属性：JVMS 只规范 static final 字段的准备期赋值，HotSpot 实测对非 final static 也执行（样本反射读取 3/3 与属性值一致）；其他 JVM 实现若不执行，回填值可能与运行期值不符。
- 写面靠指令普查兜底，反射 / JNI 写看不见，普查有疑就整轮放弃（宁误留不误删）。
- 个别巨型陷阱方法帧重算失败，整方法保持原样（v2 样本约 2% 站点），语义不变，只是该部分没还原。
- 字符串回填是 ldc 字面量（驻留池），与 clinit 期新串内容相等、实例身份不同。
- hashCode 比较对碰撞串误判相等是生成侧自认损失，equals 还原取严格语义，接收端按窗口内 String 成员调用自证。
