# analysis-grunt

## What it is

Grunt（Gruntpocalypse / Grunteon）混淆家族的 deobf 插件仓：mjbase pass-pack 静态链接 + 独立 CLI。输入混淆 jar，输出还原 jar。无宿主进程、无动态库边界，ABI 面不存在；pack 与 CLI 同源构建，跨仓混装产物由 mjbase 同源门拒绝。

代码组织遵循 [CONVENTION.md](CONVENTION.md)：以「一种残留（residue）」为单位，每种残留 = 家族知识 + 三段恒定动作（scan / rewrite / proof）。

混淆行为侧：Grunt 两代（v2 / v3）每个 obf 行为的形态描述、jasm 样例与 residue 映射见 [doc/obf-behaviors/](doc/obf-behaviors/README.md)；与 morpher 仓 `mjt-deobf/doc_ai/obf-forms/` 的分工——那边是跨家族形态库（artifact 侧形态判据与还原链序），本仓 `doc/obf-behaviors/` 是本工具行为账（逐 transformer 行为 + 配置面 + residue 现状 + 闭环缺口清单）。

## 残留一览

| 残留 | 作用 | 判据 | 等级 |
|---|---|---|---|
| `grunt.ConstantCall` | 把「全常量实参 + 纯算术目标方法」的 `invokestatic` 折叠成一条常量装载 | 目标为本容器内 `static` 方法 ∧ 实参由**紧邻的纯常量装载**产生且槽数吻合 ∧ 具体求值栈机在常量实参下跑出值（求值子集不含堆访问/调用/分配，故「求值成功」即「该路径无副作用」）；另加保守面：被调类 `<clinit>` 缺失或只剩 `return` | S1 |
| `grunt.ConstMethod` | 把「方法体只返回一个常量」的 `static` 方法（如 `static Class t() { return Long.TYPE; }`）在调用点内联展开，随后方法本体成为死码交由下游清理 | 方法体恰两条指令（装载 + 返回）∧ 装载为数值常量 / `aconst_null` / 基元类型 `.TYPE` getstatic ∧ 调用点为 `invokestatic`；宿主类 `<clinit>` 必须缺失或只做自类字段的纯算术写（跨类写会牵动初始化链，reject） | S2 |
| `grunt.FlagsStrip` | 摘掉成员上的伪 `synthetic` / `bridge` 标志（混淆器给普通成员伪造的），顺带把类版本从旧版（< 49）抬到源级默认 | 成员为 `static` ∧ 名字不含 `$`（编译器合成名必有 `$`，混淆器伪造名是搅乱的普通名）∧ 仅清 `ACC_SYNTHETIC`/`ACC_BRIDGE` 位；枚举类整体跳过 | S2 |
| `grunt.TrampolineSqueeze` | 把「跳到顺序后继」的 `goto` 置 NOP（混淆器蹦床），保留 `srcPc` 让下游实参收集看穿 | `goto`/`goto_w` 的目标恰是顺序下一条指令（等价于无操作，与混淆器版本无关）；布局缩短后新产生的蹦床同样命中 | S2 |
| `grunt.PredicateFold` | 常量操作数的分支 / `switch` 静态裁决。TAKEN 落恒真条件跳转 `[ICONST_0, IFEQ→T]`（锚改常量、分支本体原地换 op+目标、其余操作数擦除），NOT-TAKEN 锚 NOP、区域整段消失走 fallthrough | 分支操作数全部由紧邻常量装载产生 ∧ 裁决只依赖常量值与 JVM 语义；操作数区中段或分支本体有标签（合并点）则整站放弃 | S1 |
| `grunt.DeadCode` | 清掉不可达指令，并收敛因此失效的异常表项 | CFG 可达性之外的指令；可达性**含异常边**——保护区间内任一可达指令都可能抛进 handler，故 handler 以「区间∩可达集」为根迭代到不动点。全死区间删表项，端点锚在死指令上时向前吸附到存活指令 | S2 |
| `grunt.ConstValuePromote` | 把 `clinit` 里对该字段的单条常量 `putstatic` 回填成字段 `ConstantValue` 属性（并补 `ACC_FINAL`） | 字段在 `clinit` 里恰有一次常量写 ∧ 全容器无其他读写 ∧ 类型与常量种类吻合 | S1 |
| `grunt.SeedStrip` | 拆掉 `clinit` 里的 `int[]` 种子建表跨度（首条 NOP 保锚，其余擦除） | `NEWARRAY` + `PUTSTATIC` 头 + `GETSTATIC`/`IASTORE` 组的固定形状 ∧ 跨度内运算只含整长算术与转换（除法族排除，可能抛异常）；中段有标签放弃 | S2 |
| `grunt.DeadMember` | 删零引用方法、零引用私有/孤儿数组字段、空 `<clinit>`；先压池再算引用闭包 | 引用普查为零（先 `compact` 常量池，否则池常量虚撑引用）；普查按全容器对外引用全集口径——压缩失败的类照常入普查（原始字节枚举，超集宁误留），骨架都解不开的类 census-blind 整轮零删除；方法面另有纯算术 / `synthetic+bridge` / 私有门三道保守门；容器有未收敛 BSM 时 public `synthetic+bridge` 候选整档宁留（indy 目标无 CP 明文引用，dead_member.impl.md 有取证结论） | S2 |
| `grunt.BoogerAnnoStrip` | 容器里已无类引用 Booger 解密类后，摘掉残留的 `@Booger` 运行时注解条目 | 全容器无 `Booger` 类引用（解密体已被 `ConstantCall` 折成常量；引用普查有类打不开时按引用在册关死）∧ 注解描述符指向 `@Booger` | S2 |
| `grunt.IndyFold` | 把 `invokedynamic` 调用点还原成真实 `invoke`：在 libjsim 里跑 bootstrap 方法，读出 `CallSite` 的 `MethodHandle` 静态目标后改写调用点 | BSM 在常量实参下可模拟执行 ∧ 产出的 `MethodHandle` 解析为 `invokeStatic`/`invokeVirtual` 形态 ∧ 目标可解析；BoogerV2 语料实测 foldable=0（BSM 形态不在纯目标子集内），判据面向换代样本 | S3 |
| `grunt.NumberFold` | 把藏形数值常量跨度折回单条常量装载：XOR 隐匿 int 走廊（`I2L..LXOR..L2I` / `L2I..IXOR`）、基 32 `Long.parseUnsignedLong` long 跨度、`intBitsToFloat`/`longBitsToDouble` 位形包装 | 跨度逐条吻合 ∧ 操作数全为紧邻纯常量装载（基 32 键严格解析、radix 恒 32）∧ 包装引用三元组与实参类型精确吻合 ∧ 跨度中段无标签（合并点放弃） | S2 |
| `grunt.ProxyInline` | 把 static 代理方法（体 = 实参逐槽过手 + 单次 static/virtual invoke + 匹配返回）的调用点原地改指回真实目标；内联后代理变零引用死码交下游 DeadMember/DeadCode | 方法体形状（直线无标签无异常表 ∧ 实参逐槽类型/槽位精确 ∧ 转发只认 invokestatic/invokevirtual ∧ static 目标描述符全等 / virtual 目标首参=接收者）∧ 引用普查有调用点 ∧ 调用点越出 owner 类时 owner `<clinit>` 中性（初始化暴露门）；不依赖方法名/标志/家族名 | S2 |
| `grunt.FieldAccessProxy` | 把 static 字段代理方法（体 = 实参逐槽过手 + 单次字段访问 + 匹配返回）的调用点原地改回真实 get/put 字段；改回后代理变零引用死码交下游 DeadMember/DeadCode | 方法体形状（直线无标签无异常表 ∧ 实参逐槽类型/槽位精确 ∧ 唯一字段访问与描述符互证：描述符恰为按该字段该有的形状）∧ 引用普查有调用点 ∧ 调用点越出 owner 类时 owner `<clinit>` 中性（初始化暴露门，与 constant_call/proxy_inline 单源）；不依赖方法名/标志/家族名 | S2 |
| `grunt.IndyDecode` | Kasumi 混淆器 indy 调用点静态还原：提取每类 XOR 解密器 key，解密 bootstrap 静态参数（encOwner/encName/encDesc），按 mode/extra 表把 7 形态（static/virtual/字段 4 路/ctor）还原成真实 invoke/getfield/putfield/new；virtual 的接口落点走 invokeinterface，插入式站点以 spill/reload + 原地换指令保锚 | 三级分工：S2 选面（BSM 8 参描述符指纹 ∧ 解密器体形 `charAt;LDC;LDC;IXOR;IXOR;i2c`，兼容单层）→ S3 分派（mode/extra 数值表，`Era::Kasumi` 分支唯一合法落点）→ S1 定案（indy desc 与解密结果互证：static 全等 / virtual receiver 槽 / 字段 4 路形状 / ctor 返回 `Lowner;`）；key 链断 / 参数畸形 / 互证不符 / 接口无旁证整站 fail-closed（`SnakeGame-obf.jar` 语料 627 站点全还原，residual=0） | S1 |
| `grunt.StringCacheFold` | Kasumi 字符串缓存解密结果回填常量池：读缓存站点（`getstatic String[]; idx; aaload`）与直接解密站点（`getstatic byte[][]; idx; aaload; dup; arraylength; off; invokestatic ([BII)String`）锚点原地换 `ldc <明文>`、跨度其余擦除；明文来自外部 TSV 旁证（KasumiStringDumper 产出，`--tsv` 注入，内容不进仓） | S2 选面（getstatic 描述符钉 `[Ljava/lang/String;`/`[[B` ∧ 后继跨度逐条钉形，前缀口径）→ S3 Era 门（TSV 属换代表面，`Era::Kasumi` 分支唯一合法落点）→ S1 定案（明文表 `(owner,key,idx)` 精确命中）；旁证缺席/不可读整站 fail-closed、表无该条目按树版静默跳过 + `noPlaintext` 记账——S1 无旁证不可定案（明文本身就是要回填的内容，树版空映射即整 pass 跳过，语义同源）（`SnakeGame-obf.jar` 语料单跑 168 站点回填 84+84；专链上 `IndyDecode` 先行解锁 3 站 → 171=84+87，与树版逐项同；residual=0） | S1 |
| `grunt.MetadataStrip` | Kasumi 元数据残留清理三面：`@dev/kasumi/internal/Metadata` 注解摘除（类/字段/方法三张挂载面，其余注解保留）、水印字段整删（带注解字段 ∨ ConstantValue 为水印串：含 `PROTECTED BY Kasumi` / 含 `Gruntpocalypse` / 前缀 `kasumi_`，树版三判据逐字口径）、反 LLM 注入资源删除（`META-INF/Kasumi/*.md` 前缀 + 内容探针五选一，本仓首个资源面写动词）；方法体不碰（引导/解密器交 indy 还原与下游死码清理） | S2 形状钉面（注解描述符全等 ∧ 资源 URI 前缀+`.md` 形状 ∧ 水印串形状）→ S3 Era 门（三面判据全是 Kasumi 版本实现选择，`Era::Kasumi` 分支唯一合法落点，未判整面关闭一个资源不动）→ S1 互证（资源删除要内容探针坐实注入物，形状命中而探针缺席保留 + `resourceCandidates` 盘点；水印字段要 ConstantValue 内容命中）；注解槽过深度预检（mjbase #158），解不开 fail-closed 不摘 + `annoBlocked` 记账；proof 双门：类数不增 + 资源对账（数只减 + 删除清单逐条不在场）（`SnakeGame-obf.jar` 语料 25 注解 + 10 水印字段 + 4 资源全清，residual=0） | S1 |
| `grunt.StringPoolFold` | Grunt v2 `StringEncrypt` / v3 `StringArrayedEncrypt` 字符串池读点折回：`(getstatic String[] 池; 紧邻常量下标; aaload)` 三元组原地换 `ldc <明文>`（srcPc 保留、跨度其余擦除）；明文由池链静态复算——clinit 分配前导四连 → arrayInit 块序列（v2 literal `ldc+toCharArray` / v3 arrayed `NEWARRAY T_CHAR+CASTORE`，按常量 KIND 判形）→ `([CJI)String` 解密器（classKey 烧体、seed/key 全装载期常量，自逆 XOR 流静态重放），无外部旁证；直线静态蹦床（indy 还原后产物形）跟一跳，indy 藏面 fail-closed；池字段/arrayInit/解密器不在此删（仍被前导引用，压池属后续批次） | S2 选面（三元组形状 + 池字段描述符）→ S1 定案（池链三件套逐字钉形 + 52 条解密器核窗口唯一命中 + 槽位互证 + 逐下标解密）；任一环缺席/歧义/被混淆打碎整类 fail-closed（boar 语料单跑 0 折叠——Grunteon 全链把 arrayInit 调用藏进 indy 蹦床，fail-closed 正确；`IndyRestore` 上游接通后 19342 站点 / 19244 折叠，residual=0） | S1 |
| `grunt.IndyRestore` | Grunt v2 `InvokeDynamic` / v3 `ReferenceObfuscate` indy 调用点直接解密回填：simple 形三段串密文（点分 owner/name/desc + kind 位）按 `encrypt` 自逆 XOR 纯算术直解；heavy 形 owner 经 BSM 体 LDC generalKey 解一层、真目标由 owner 类 `@Metadata`（mv≥100 门）d1/d2 数组 `data1 % 59557 == magic2` 自证命中后解出 `name<>desc`；调用点**纯原地**换真实 invoke（static → invokestatic，virtual → invokevirtual/invokeinterface，receiver 槽就是 indy 首参不插不删）；BSM 名随机不进判据，key 定位走三路键链（同名解密器直连 → BSM 体跟随 → 名字族推导 c1/c2，覆盖 NativePipeline 壳死现场） | S2 选面（simple/heavy 7 参 BSM desc 指纹 ∧ 解密器窗口形状，v2/v3 逐字同源）→ S1 定案（解密产物与 indy desc 互证：static 参数段全等 / virtual 首参严格 Object + 其余逐位一致；heavy 另加 `data1 % 59557 == magic2` 完整性自证 + `name<>desc` 严格两段拆分；kind 位只认 0/1）→ S3 Era 门（heavy 的 @Metadata 数组解释版本敏感，`Era::Grunt3` 双指纹判出才激活；simple 面全结构自证不挂 Era）；接口落点容器普查优先 + `--jdk` 穷举集双向作证，两无 fail-closed；材料通道（RuntimeMaterial 运行期材料字段）静态不可解 fail-closed + `materialLane` 记账（boar 语料 78154 指纹站点还原 30924=static 29847+virtual 1077，failed=0，residual=47230 如实挂账；组合链上 StringPoolFold 0 → 19244 折叠；`-Xverify:all` 逐类失败集与输入全等） | S1 |

`S1` = 语义后果：混淆器为了保持功能必须如此，改了自己就坏。`S2` = 结构性清理：判据成立即安全，但混淆器换代可能换形态。`S3` = 面向未来形态的预留面：当前语料无站点，判据按形态写、按换代样本验收。判据不依赖方法名、签名形状、所在类或注入模板，所以 Grunt 换代不需要改这一行。

## 链序

主线（15 pass，BoogerV2 语料整链验收；FieldAccessProxy 为后续批次接入）：

```
grunt.TrampolineSqueeze,grunt.NumberFold,grunt.StringPoolFold,grunt.IndyFold,grunt.ConstMethod,
grunt.ProxyInline,grunt.FieldAccessProxy,grunt.ConstantCall,grunt.PredicateFold,grunt.SeedStrip,
grunt.ConstValuePromote,grunt.DeadCode,grunt.DeadMember,grunt.FlagsStrip,
grunt.BoogerAnnoStrip
```

三条硬约束：

- **`TrampolineSqueeze` 排在实参收集面之前**：它把蹦床 `goto` 置 NOP 并保
  `srcPc`，`NumberFold` / `ConstantCall` / `PredicateFold` 的紧邻常量收集
  才能看穿被压住的操作数区。
- **`NumberFold` 排在实参收集面之前**：它把藏形数值跨度折成纯常量装载，
  `ConstantCall` / `PredicateFold` / `ConstMethod` 的实参收集只认纯常量
  装载——藏形跨度夹在实参区里，下游整站失明。
- **`ConstantCall` 排在折叠/清死之前**：它把调用点变成常量字面量，下游的
  `PredicateFold` / `ConstValuePromote` / 死码删除才有东西可折可清。

`StringPoolFold` 与 `NumberFold` 同组（值还原面），排在实参收集面之前：
折出的 `ldc <明文>` 可继续喂 `ConstantCall` / `PredicateFold` 的常量实参
收集；判据自证（池链三件套 + 解密器窗口），无前置依赖，三元组内 NOP
跳过兼容 squeeze 遗留。indy 藏面（Grunteon 全链产物）在其上游
`grunt.IndyRestore` 把 indy 还原成直线静态蹦床后接通（boar 语料实证：
单跑 0 折叠 → 链上 19244 折叠，见 [doc/string-pool-fold.impl.md](doc/string-pool-fold.impl.md)
与 [doc/indy-restore.impl.md](doc/indy-restore.impl.md) 语料账本）；
Grunteon 现场的 Grunt 面专链为 `grunt.IndyRestore,grunt.StringPoolFold`。

形状纪律（`PredicateFold` 写回契约的前提，见 doc/predicate-fold.impl.md）：
改写不得引入无条件跳转、不得新增合并点——HotSpot 线性走查要求无条件跳转
之后的段首带帧，本 pass 不再生 SMT。TAKEN 的 `IFEQ` 目标是原分支目标（本
来就有帧），NOT-TAKEN 留下的死块带原帧，中间态合法；`DeadCode` 紧随清理
是推荐序而非性质约束。

单轮跑到收敛要重复整条链：`DeadCode` 擦除缩短布局后，原本跨过死区的
`goto` 会变成「跳到顺序后继」的新蹦床（grunt2 第二轮实测 4193 处），
`ConstValuePromote` 回填 `ConstantValue` 后 `clinit` 变空，也会解锁新的
`DeadMember` 删除面。判据是 `PredicateFold` 站点归零。

**Kasumi 专面不入主线，走专链**：`grunt.IndyDecode`、`grunt.StringCacheFold` 与
`grunt.MetadataStrip` 由 `probeEra` 判家（`Era::Kasumi` 才激活），与 BoogerV2
主线互不重叠——Kasumi BSM 的 8 参签名 + find* 分派不在 `IndyFold` 的纯目标子
集内，同一 jar 上两者不争站点。专链三 pass、链序固定（先还原再清理，
`MetadataStrip` 收尾）：

```
grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip
```

`--jdk`（接口旁证）与 `--tsv`（明文旁证，内容不进仓）见 [Run](#run) 与
[doc/kasumi-verify.md](doc/kasumi-verify.md)（端到端五项门逐项数字与和树版
口径的差异点）。**通用主线可叠加在专链产物上**（前置条件已解除）：K4 曾取证
`DeadMember` 引用普查漏计压缩失败类（cp_compact 封闭集拒绝的 Raw 属性类，
Kasumi 改写产物 16/25 类）的对外引用，专链产物上误删（语料 `t` 类 93 → 0
方法，运行期 `NoSuchMethodError`）。普查已按「全容器对外引用全集」口径修复：
压缩失败的类按原始字节照常入普查（枚举超集宁误留），连骨架都解不开的类
census-blind 整轮零删除；通用链 × 专链组合与通用链单跑复验通过，修复前后
数字与 JVM 实调证据见 kasumi-verify.md §3。


## 全局不变量

- **类数不增**：任何残留都不得新增类；`proof` 段每次校验。资源面同理**只
  减不增**（资源删除残留另加删除清单逐条对账，见 proof_toolbox 的资源对
  账原语）。
- **origin 永不改写**：输入 jar 只读，产物另存。
- **fail-closed**：判据不全整站点跳过，不做猜测性改写；拒绝原因进 `rejects` 供 trace。
- **引用普查全容器口径**：跨类引用普查是删动词的前置证据面，静默跳过的类就是蒸发的引用证据——解不开的类按原始字节照常入普查（超集宁误留），连骨架都解不开则整轮改写放弃（DeadMember census-blind / SeedStrip 与 ConstValuePromote 普查不完整整轮放弃 / BoogerAnnoStrip 引用门按引用在册关死）。
- **改写粒度 = 类**：一个类内任一方法 `lower` 失败，整类放弃回写。
- **擦除要给旧位置留落点**：`lower` 的属性重定位按旧 pc 精确命中 `pcmap`，命中不了就整体失败。所以「能原地改写一条指令」就绝不用「插一条 + 删一条」——插入路径会把 `srcPc` 清成 `kNoPc`，锚在被删位置的调试行/栈帧全部失去落点。详见 [doc/constant-call.impl.md](doc/constant-call.impl.md) 的 Rewrite 章。
- **改前先往返**：`lower` 不是 `promote` 的恒等逆。改写任何方法前，先在未改动的独立会话上跑一遍 `promote → lower`；跑不完的方法整方法跳过，别把它的编码边界算成改写缺陷。

## Build

mjbase 经 luiox-repo 中心包仓消费（`xmake f` 自动添加仓源并拉取 mjbase /
libjbc / libca 及传递闭包），无需本地 mjbase 检出。libjsim 仍是源码级
header-only 依赖：默认找兄弟目录 `../libjsim`，用 `LIBSIM_DIR` 覆盖。

```sh
xmake f
xmake build grunt_cli
```

## Run

```sh
./build/windows/x64/release/grunt_cli.exe -i in.jar -o out.jar \
  -p grunt.TrampolineSqueeze,grunt.IndyFold,grunt.ConstMethod,grunt.ProxyInline,grunt.FieldAccessProxy,grunt.ConstantCall,grunt.PredicateFold,grunt.SeedStrip,grunt.ConstValuePromote,grunt.DeadCode,grunt.DeadMember,grunt.FlagsStrip,grunt.BoogerAnnoStrip
```

直接调构建产物，别用 `xmake run grunt_cli --`：xmake 会吞掉/改写一部分参数（引号与逗号尤其容易走样）。

Kasumi 专链用法（专链无 preset 机制，仓内链注册形态就是 `-p` 逗号分隔名，标准命令行即装配）：

```sh
./build/windows/x64/release/grunt_cli.exe -i in.jar -o out.jar \
  -p grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip \
  --jdk <jdk17 根目录> --tsv <KasumiStringDumper 产的明文表>
```

`-p` 逗号分隔 pass 名（`grunt.` 前缀）；`-c pack.json` 把配置文本整体交给 pack 解析；`--list` 列出全部可用 pass；`--jdk <JDK 根目录>` 是 `grunt.IndyDecode` 的接口旁证注入面（扫 jmods/lib 收集接口全集，Era=Kasumi 才激活；不传则容器外落点的 virtual 站点 fail-closed）；`--tsv <明文表>` 是 `grunt.StringCacheFold` 的 TSV 旁证注入面（KasumiStringDumper 四列映射，Era=Kasumi 才激活；缺失/不可读旁证降级 fail-closed）。

单轮不收敛，要整链重复跑（输出喂回输入）直到 `PredicateFold` 站点归零：

```sh
cur=in.jar; for i in 1 2 3 4; do
  ./build/windows/x64/release/grunt_cli.exe -i "$cur" -o "out$i.jar" -p "$CHAIN" || break
  cur="out$i.jar"
done
```

排查「某个站点为什么没折」时开 `GRUNT_TRACE=1`，站点级统计会打到 stderr；`GRUNT_DEBUG_FOLD=1` 逐条打「哪个调用点折成了什么值」，`GRUNT_DEBUG_ERASE=1` 打改写/lower/写回的失败明细，`GRUNT_DEBUG_PCREFS=1` 打「哪些属性行指着将被擦除的 pc」。

**顺序**：见上文[链序](#链序)。`grunt.TrampolineSqueeze` 先于实参收集面，`grunt.ConstantCall` 先于折叠/清死；单轮不收敛，整链要重复跑到 `PredicateFold` 站点归零。

## Test

```sh
xmake f --with_tests=y && xmake build grunt_unittest && xmake run grunt_unittest
```

`test/unittest/constant_call_test.cpp` 覆盖三段判据：正例（短形态值、需落池的值）+ 逐条反证（非 static / void / synchronized / 实参非常量 / 目标不纯 / 目标不在容器 / 类初始化非空），改写侧断言「调用消失、字面量出现、实参装载全清」，proof 侧断言类数门。`flags_strip_test.cpp` 覆盖标志判据（清位面、`$` 名保留、旧类版本、枚举跳过、逐位精确），`const_method_test.cpp` 覆盖常量方法判据（基元 `.TYPE` 内联、数值/ null 体、非 `.TYPE` getstatic 拒绝、宿主 `<clinit>` 纯性门），`predicate_fold_test.cpp` 覆盖常量谓词判据（分支裁决、`switch` 键裁决、操作数区中段有标签则放弃、改写后帧链自洽），`number_fold_test.cpp` 覆盖藏形数值判据（XOR 走廊两式、基 32 long、位形包装、radix/键面/被调/中段标签反证、折叠后不可再成站点），`proxy_inline_test.cpp` 覆盖代理方法判据（static/virtual 目标、宽类型槽走、零引用静默、体形/描述符/synchronized/初始化暴露反证、调用点原地改指、改写后再扫站点归零），`indy_decode_test.cpp` 覆盖 Kasumi indy 判据（static 原地换 invokestatic、virtual 插 spill/checkcast 后走 invokeinterface、Era 未判 / key 链断 / 参数畸形 / desc 互证不符 / 接口无旁证五态拒绝且容器字节逐位不动、proof 类数门 + residual 归零），`metadata_strip_test.cpp` 覆盖 Kasumi 元数据三面判据（注解三面摘除且异名注解保留、水印字段删而普通 ConstantValue 字段留、资源命中删而候选/前缀不符逐位留、Era 未判整面关闭字节逐位不动、敌意深嵌注解槽 fail-closed 不摘、proof 类数门 + 资源对账门 + 三面 residual；水印字段经 RawAttr 程序化补挂 ConstantValue——jasm 表达不了该属性）。夹具一律用 jasm 源码现场汇编（`JvmJasmService::assemble`），不落二进制、不依赖语料目录。

语料回归：

```sh
xmake build grunt_cli && ./test/corpus-regress.sh
```

对语料目录里的每个 jar 跑完整残留链，检查退出码、输出 jar 结构完整、**类数不增**，并在有 JDK 时对产物做逐类加载校验（`test/tools/VerifyJar.java`）。语料回归的判定方式不是「产物和某个黄金字节一致」，而是「输入与产物各自过一遍校验器，失败集合必须相同」——样本普遍缺外部依赖，绝对失败数为零不是可达目标，**没有新增失败**才是。Kasumi 专链的语料回归：`ONLY=SnakeGame-obf PASSES='grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip' EXTRA_ARGS="--jdk <jdk17 根目录> --tsv <TSV 路径>" ./test/corpus-regress.sh`。

## Doc index

- [doc/README.md](doc/README.md)：索引与口径分工。
- [doc/obf-behaviors/README.md](doc/obf-behaviors/README.md)：grunt 行为账总表（行为 × residue 状态）与闭环缺口清单（P1/P2/P3）。
- [doc/obf-behaviors/grunt-v2-behaviors.md](doc/obf-behaviors/grunt-v2-behaviors.md)：v2（Gruntpocalypse 2.x）35 个 transformer 逐行为描述 + 16 个已验证 jasm 样例。
- [doc/obf-behaviors/grunt-v3-behaviors.md](doc/obf-behaviors/grunt-v3-behaviors.md)：v3（Grunteon）39 个 transformer + GlslObfuscator 插件 + NativePipeline 阶段逐行为描述 + 11 个已验证 jasm 样例。
- [doc/constant-call.md](doc/constant-call.md) / [.impl.md](doc/constant-call.impl.md)：常量调用。
- [doc/flags-strip.md](doc/flags-strip.md) / [.impl.md](doc/flags-strip.impl.md)：伪 `synthetic`/`bridge` 标志。
- [doc/const-method.md](doc/const-method.md) / [.impl.md](doc/const-method.impl.md)：常量方法。
- [doc/trampoline-squeeze.md](doc/trampoline-squeeze.md) / [.impl.md](doc/trampoline-squeeze.impl.md)：蹦床 `goto`。
- [doc/predicate-fold.md](doc/predicate-fold.md) / [.impl.md](doc/predicate-fold.impl.md)：常量谓词裁决。
- [doc/dead-code.md](doc/dead-code.md) / [.impl.md](doc/dead-code.impl.md)：死码与异常表可达性。
- [doc/const-value-promote.md](doc/const-value-promote.md) / [.impl.md](doc/const-value-promote.impl.md)：常量字段回填。
- [doc/seed-strip.md](doc/seed-strip.md) / [.impl.md](doc/seed-strip.impl.md)：种子建表跨度。
- [doc/dead-member.md](doc/dead-member.md) / [.impl.md](doc/dead-member.impl.md)：死成员与引用闭包。
- [doc/booger-anno-strip.md](doc/booger-anno-strip.md) / [.impl.md](doc/booger-anno-strip.impl.md)：残留 `@Booger` 注解。
- [doc/number-fold.md](doc/number-fold.md) / [.impl.md](doc/number-fold.impl.md)：藏形数值常量跨度。
- [doc/indy-fold.md](doc/indy-fold.md) / [.impl.md](doc/indy-fold.impl.md)：indy 解密调用点。
- [doc/proxy-inline.md](doc/proxy-inline.md) / [.impl.md](doc/proxy-inline.impl.md)：代理方法内联。
- [doc/indy-decode.md](doc/indy-decode.md) / [.impl.md](doc/indy-decode.impl.md)：Kasumi indy 静态还原。
- [doc/string-cache-fold.md](doc/string-cache-fold.md) / [.impl.md](doc/string-cache-fold.impl.md)：Kasumi 字符串缓存回填。
- [doc/metadata-strip.md](doc/metadata-strip.md) / [.impl.md](doc/metadata-strip.impl.md)：Kasumi 元数据与水印清理。
- [doc/kasumi-verify.md](doc/kasumi-verify.md)：Kasumi 专链端到端复现记录（五项门逐项数字、与树版口径差异点、通用链取证）。

口径分工：`doc/` 只写**通用技术形态**与实现；具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
