# analysis-grunt

## What it is

Grunt（Gruntpocalypse / Grunteon）混淆家族的 deobf 插件仓：mjbase pass-pack 静态链接 + 独立 CLI。输入混淆 jar，输出还原 jar。无宿主进程、无动态库边界，ABI 面不存在；pack 与 CLI 同源构建，跨仓混装产物由 mjbase 同源门拒绝。

代码组织遵循 [CONVENTION.md](CONVENTION.md)：以「一种残留（residue）」为单位，每种残留 = 家族知识 + 三段恒定动作（scan / rewrite / proof）。

## 残留一览

| 残留 | 作用 | 判据 | 等级 |
|---|---|---|---|
| `grunt.ConstantCall` | 把「全常量实参 + 纯算术目标方法」的 `invokestatic` 折叠成一条常量装载 | 目标为本容器内 `static` 方法 ∧ 实参由**紧邻的纯常量装载**产生且槽数吻合 ∧ 具体求值栈机在常量实参下跑出值（求值子集不含堆访问/调用/分配，故「求值成功」即「该路径无副作用」）；另加保守面：被调类 `<clinit>` 缺失或只剩 `return` | S1 |

`S1` = 语义后果：混淆器为了保持功能必须如此，改了自己就坏。判据不依赖方法名、签名形状、所在类或注入模板，所以 Grunt 换代不需要改这一行。

## 全局不变量

- **类数不增**：任何残留都不得新增类；`proof` 段每次校验。
- **origin 永不改写**：输入 jar 只读，产物另存。
- **fail-closed**：判据不全整站点跳过，不做猜测性改写；拒绝原因进 `rejects` 供 trace。
- **改写粒度 = 类**：一个类内任一方法 `lower` 失败，整类放弃回写。
- **擦除要给旧位置留落点**：`lower` 的属性重定位按旧 pc 精确命中 `pcmap`，命中不了就整体失败。所以「能原地改写一条指令」就绝不用「插一条 + 删一条」——插入路径会把 `srcPc` 清成 `kNoPc`，锚在被删位置的调试行/栈帧全部失去落点。详见 [doc/constant-call.impl.md](doc/constant-call.impl.md) 的 Rewrite 章。
- **改前先往返**：`lower` 不是 `promote` 的恒等逆。改写任何方法前，先在未改动的独立会话上跑一遍 `promote → lower`；跑不完的方法整方法跳过，别把它的编码边界算成改写缺陷。

## Build

需要本地 mjbase 检出且含 plugin 接口（plugin-api 分支及以上），默认找 `../mjbase-plugin-api`，用 `MJBASE_DIR` 覆盖：

```sh
MJBASE_DIR=/path/to/mjbase-plugin-api xmake f
xmake build grunt_cli
```

## Run

```sh
xmake run grunt_cli -- -i in.jar -o out.jar -p grunt.ConstantCall
```

`-p` 逗号分隔 pass 名（`grunt.` 前缀）；`-c pack.json` 把配置文本整体交给 pack 解析；`--list` 列出全部可用 pass。

排查「某个站点为什么没折」时开 `GRUNT_TRACE=1`，站点级统计会打到 stderr；`GRUNT_DEBUG_FOLD=1` 逐条打「哪个调用点折成了什么值」，`GRUNT_DEBUG_ERASE=1` 打改写/lower/写回的失败明细，`GRUNT_DEBUG_PCREFS=1` 打「哪些属性行指着将被擦除的 pc」。

**顺序**：`grunt.ConstantCall` 应排在常量折叠/死码清理之前——它把调用点变成常量字面量，下游的 `ConstantFolder` / `ConstantSwitchFolder` / 死码删除才有东西可折。

## Test

```sh
xmake f --with_tests=y && xmake build grunt_unittest && xmake run grunt_unittest
```

`test/unittest/constant_call_test.cpp` 覆盖三段判据：正例（短形态值、需落池的值）+ 逐条反证（非 static / void / synchronized / 实参非常量 / 目标不纯 / 目标不在容器 / 类初始化非空），改写侧断言「调用消失、字面量出现、实参装载全清」，proof 侧断言类数门。夹具一律用 jasm 源码现场汇编（`JvmJasmService::assemble`），不落二进制、不依赖语料目录。

语料回归：

```sh
xmake build grunt_cli && ./test/corpus-regress.sh
```

对语料目录里的每个 jar 跑完整残留链，检查退出码、输出 jar 结构完整、**类数不增**，并在有 JDK 时对产物做逐类加载校验（`test/tools/VerifyJar.java`）。语料回归的判定方式不是「产物和某个黄金字节一致」，而是「输入与产物各自过一遍校验器，失败集合必须相同」——样本普遍缺外部依赖，绝对失败数为零不是可达目标，**没有新增失败**才是。

## Doc index

- [doc/README.md](doc/README.md)：索引与口径分工。
- [doc/constant-call.md](doc/constant-call.md)：常量调用这一形态的技术描述（artifact 侧）。
- [doc/constant-call.impl.md](doc/constant-call.impl.md)：本仓怎么还原它（Scan / Rewrite / Proof 三章）。

口径分工：`doc/` 只写**通用技术形态**与实现；具体样本的取证过程（jar、类名、指令偏移、折叠期望值）记在仓外研究笔记，不进本仓。
