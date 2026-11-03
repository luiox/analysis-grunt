# analysis-grunt

## What it is

Grunt（Gruntpocalypse / Grunteon）混淆家族的 deobf 插件仓：mjbase pass-pack 静态链接 + 独立 CLI。输入混淆 jar，输出还原 jar。无宿主进程、无动态库边界，ABI 面不存在；pack 与 CLI 同源构建，跨仓混装产物由 mjbase 同源门拒绝。

代码组织遵循 [CONVENTION.md](CONVENTION.md)：以「一种残留（residue）」为单位，每种残留 = 家族知识 + 三段恒定动作（scan / rewrite / proof）。

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
| `grunt.DeadMember` | 删零引用方法、零引用私有/孤儿数组字段、空 `<clinit>`；先压池再算引用闭包 | 引用普查为零（先 `compact` 常量池，否则池常量虚撑引用）；方法面另有纯算术 / `synthetic+bridge` / 私有门三道保守门，`ACC_BRIDGE` 成员零引用也豁免（虚分派经桥，引用普查看不见） | S2 |
| `grunt.BoogerAnnoStrip` | 容器里已无类引用 Booger 解密类后，摘掉残留的 `@Booger` 运行时注解条目 | 全容器无 `Booger` 类引用（解密体已被 `ConstantCall` 折成常量）∧ 注解描述符指向 `@Booger` | S2 |
| `grunt.IndyFold` | 把 `invokedynamic` 调用点还原成真实 `invoke`：在 libjsim 里跑 bootstrap 方法，读出 `CallSite` 的 `MethodHandle` 静态目标后改写调用点 | BSM 在常量实参下可模拟执行 ∧ 产出的 `MethodHandle` 解析为 `invokeStatic`/`invokeVirtual` 形态 ∧ 目标可解析；BoogerV2 语料实测 foldable=0（BSM 形态不在纯目标子集内），判据面向换代样本 | S3 |
| `grunt.NumberFold` | 把藏形数值常量跨度折回单条常量装载：XOR 隐匿 int 走廊（`I2L..LXOR..L2I` / `L2I..IXOR`）、基 32 `Long.parseUnsignedLong` long 跨度、`intBitsToFloat`/`longBitsToDouble` 位形包装 | 跨度逐条吻合 ∧ 操作数全为紧邻纯常量装载（基 32 键严格解析、radix 恒 32）∧ 包装引用三元组与实参类型精确吻合 ∧ 跨度中段无标签（合并点放弃） | S2 |
| `grunt.ProxyInline` | 把 static 代理方法（体 = 实参逐槽过手 + 单次 static/virtual invoke + 匹配返回）的调用点原地改指回真实目标；内联后代理变零引用死码交下游 DeadMember/DeadCode | 方法体形状（直线无标签无异常表 ∧ 实参逐槽类型/槽位精确 ∧ 转发只认 invokestatic/invokevirtual ∧ static 目标描述符全等 / virtual 目标首参=接收者）∧ 引用普查有调用点 ∧ 调用点越出 owner 类时 owner `<clinit>` 中性（初始化暴露门）；不依赖方法名/标志/家族名 | S2 |

`S1` = 语义后果：混淆器为了保持功能必须如此，改了自己就坏。`S2` = 结构性清理：判据成立即安全，但混淆器换代可能换形态。`S3` = 面向未来形态的预留面：当前语料无站点，判据按形态写、按换代样本验收。判据不依赖方法名、签名形状、所在类或注入模板，所以 Grunt 换代不需要改这一行。

## 链序

主线（13 pass，BoogerV2 语料整链验收）：

```
grunt.TrampolineSqueeze,grunt.NumberFold,grunt.IndyFold,grunt.ConstMethod,
grunt.ProxyInline,grunt.ConstantCall,grunt.PredicateFold,grunt.SeedStrip,
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

形状纪律（`PredicateFold` 写回契约的前提，见 doc/predicate-fold.impl.md）：
改写不得引入无条件跳转、不得新增合并点——HotSpot 线性走查要求无条件跳转
之后的段首带帧，本 pass 不再生 SMT。TAKEN 的 `IFEQ` 目标是原分支目标（本
来就有帧），NOT-TAKEN 留下的死块带原帧，中间态合法；`DeadCode` 紧随清理
是推荐序而非性质约束。

单轮跑到收敛要重复整条链：`DeadCode` 擦除缩短布局后，原本跨过死区的
`goto` 会变成「跳到顺序后继」的新蹦床（grunt2 第二轮实测 4193 处），
`ConstValuePromote` 回填 `ConstantValue` 后 `clinit` 变空，也会解锁新的
`DeadMember` 删除面。判据是 `PredicateFold` 站点归零。


## 全局不变量

- **类数不增**：任何残留都不得新增类；`proof` 段每次校验。
- **origin 永不改写**：输入 jar 只读，产物另存。
- **fail-closed**：判据不全整站点跳过，不做猜测性改写；拒绝原因进 `rejects` 供 trace。
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
  -p grunt.TrampolineSqueeze,grunt.IndyFold,grunt.ConstMethod,grunt.ProxyInline,grunt.ConstantCall,grunt.PredicateFold,grunt.SeedStrip,grunt.ConstValuePromote,grunt.DeadCode,grunt.DeadMember,grunt.FlagsStrip,grunt.BoogerAnnoStrip
```

直接调构建产物，别用 `xmake run grunt_cli --`：xmake 会吞掉/改写一部分参数（引号与逗号尤其容易走样）。

`-p` 逗号分隔 pass 名（`grunt.` 前缀）；`-c pack.json` 把配置文本整体交给 pack 解析；`--list` 列出全部可用 pass。

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

`test/unittest/constant_call_test.cpp` 覆盖三段判据：正例（短形态值、需落池的值）+ 逐条反证（非 static / void / synchronized / 实参非常量 / 目标不纯 / 目标不在容器 / 类初始化非空），改写侧断言「调用消失、字面量出现、实参装载全清」，proof 侧断言类数门。`flags_strip_test.cpp` 覆盖标志判据（清位面、`$` 名保留、旧类版本、枚举跳过、逐位精确），`const_method_test.cpp` 覆盖常量方法判据（基元 `.TYPE` 内联、数值/ null 体、非 `.TYPE` getstatic 拒绝、宿主 `<clinit>` 纯性门），`predicate_fold_test.cpp` 覆盖常量谓词判据（分支裁决、`switch` 键裁决、操作数区中段有标签则放弃、改写后帧链自洽），`number_fold_test.cpp` 覆盖藏形数值判据（XOR 走廊两式、基 32 long、位形包装、radix/键面/被调/中段标签反证、折叠后不可再成站点），`proxy_inline_test.cpp` 覆盖代理方法判据（static/virtual 目标、宽类型槽走、零引用静默、体形/描述符/synchronized/初始化暴露反证、调用点原地改指、改写后再扫站点归零）。夹具一律用 jasm 源码现场汇编（`JvmJasmService::assemble`），不落二进制、不依赖语料目录。

语料回归：

```sh
xmake build grunt_cli && ./test/corpus-regress.sh
```

对语料目录里的每个 jar 跑完整残留链，检查退出码、输出 jar 结构完整、**类数不增**，并在有 JDK 时对产物做逐类加载校验（`test/tools/VerifyJar.java`）。语料回归的判定方式不是「产物和某个黄金字节一致」，而是「输入与产物各自过一遍校验器，失败集合必须相同」——样本普遍缺外部依赖，绝对失败数为零不是可达目标，**没有新增失败**才是。

## Doc index

- [doc/README.md](doc/README.md)：索引与口径分工。
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

口径分工：`doc/` 只写**通用技术形态**与实现；具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
