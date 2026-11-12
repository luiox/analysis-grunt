# Kasumi 端到端复现记录（SnakeGame-obf）

> 样本：`SnakeGame-obf.jar`（run-lab 语料位；= morpher 树版样本
> SnakeGame-obf (1).jar，来源与 jar 形态见 doc/indy-decode.md），
> 582,974 B，sha256 `c3481fb16f66ea9fade86e601a73b651db984666de18fe356bb439cc874f77fc`。
> 专链：`grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip`（MetadataStrip 收尾，
> 先还原再清理）。jar / TSV / 产物一律不进仓，本文只记数字与来源。
> 对位：morpher 树版 `mjt-deobf/test/kasumi/verify.md`（origin/main）。
> 记录批次：K4 收官（代码基线 `6b38297`，unittest 155/155）；§3 普查修复批次
> 补记（代码基线 `f9b25ce`，unittest 158/158）。

## 0. 还原命令（两步：dump 字符串 + 专链）

```sh
# 1. 导出字符串明文映射（dumper 为 morpher 树版 Java 工具，源不进 grunt 仓）
javac -encoding UTF-8 KasumiStringDumper.java
java -cp . KasumiStringDumper SnakeGame-obf.jar > SnakeGame-obf.strings.tsv

# 2. Kasumi 专链（--jdk 接口旁证 + --tsv 明文旁证；直接调产物，勿走 xmake run）
grunt_cli -i SnakeGame-obf.jar -o SnakeGame-deobf.jar \
  -p grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip \
  --jdk <jdk17 根目录> \
  --tsv SnakeGame-obf.strings.tsv
```

- TSV：222 行（7 类 × {a,b,c, byte:a, byte:b} 非空槽位），本轮重产，sha256
  `9384eb3fd67c556c2a23143f98de7a1dd1d62c7ce2ac255477df6d4c9028da3d`。
- 语料回归一门：

```sh
CORPUS=<run-lab> ONLY=SnakeGame-obf \
  PASSES='grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip' \
  EXTRA_ARGS="--jdk <jdk17 根目录> --tsv <TSV 路径>" \
  ./test/corpus-regress.sh     # 实测：ok SnakeGame-obf，语料 1 个，失败 0 个
```

## 1. 五项门（逐项数字，绑产物）

产物：`SnakeGame-deobf.jar` 529,743 B，sha256
`43e089b19fc78c8acc62c0c0c0bb73fcace45f78b0d09ebfa537755371683106`
（trace 逐行对应本节数字）。

| # | 门 | 数字 |
|---|---|---|
| ① | indy 站点 | scan `total=627 sites=627 rejects=0`（六桶全 0）；rewrite `sites=627 failed=0`（static=478 / virtual=72 / getfield=4 / putfield=4 / getstatic=28 / putstatic=34 / new=7）；proof `residual=0`——与树版逐项同 |
| ② | 字符串回填 | scan `total-shape=174 sites=171 no-plain=3`；rewrite `cache-read=84 direct-decrypt=87 failed=0`；proof `residual=0`——合计 171，与树版 84+87 逐项同（单 pass 隔离口径 168=84+84，见 §2-2） |
| ③ | 三面残留 | scan `annos=25 fields=10 resources=4 candidates=0 anno-blocked=0`；rewrite `failed=0`；proof `residual(a/f/r)=0/0/0` |
| ④ | 类数 / 条目 | 类数 25 → 25（不变）；文件条目 30 → 26（恰删 4 个反 LLM 资源，只减成立） |
| ⑤ | VerifyJar | 输入 / 输出各 `classes=25 ok=25 failed=0`，失败集逐类同（双侧零失败） |

## 2. 与树版口径的差异点

1. **功能运行不过（帧初始帧掉 receiver）**：树版 deobf jar 正常启动到超时；
   本仓专链产物五项门全过但 `java -cp` 实跑 `VerifyError`（`h.c()V` 分支目标
   帧不一致）。定位：改写后的 StackMapTable 以 `full_frame(@0, locals={})` 开头
   ——实例方法的初始帧掉了 receiver 槽（树版保原帧 `same off=0`，初始帧由
   JVM 按描述符推导）。这是帧重序列化面的缺陷（mjbase 侧），五项门照不到：
   `VerifyJar` 用 `Class.forName(n, false, cl)` 懒链接，方法级校验延迟到首次
   执行才触发，类加载门全绿掩盖帧错。**挂账 mjbase 帧序列化**；修好后本节
   应以实跑 rc=124 复验替换。
2. **字符串站点口径（168 vs 171）**：单 pass 隔离验收 168（cache 84 + direct
   84，`total-shape=170 no-plain=2`）；专链上 `IndyDecode` 先行把 3 个本被
   indy 化的站点还原成裸形状（`total-shape=174`、`no-plain=3`），回填 171 =
   84+87，恰与树版逐项同——组合链优于单跑，是链序「先还原再清理」的实证。
3. **javap 可读性**：树版 kasumi pipeline 清非法 `Signature`（javap 恢复
   可读）；本仓 `MetadataStrip` 三面（注解 / 水印 / 资源）不含 Signature 面，
   产物对 `javap -v` 仍崩（`IllegalStateException: ! !`）。Signature 清理
   面不在 K0~K4 范围，遗留。
4. **条目口径**：物理 zip 条目 30 → 30（删除 4 资源 + jar 导出器对存留路径
   补 4 个目录条目）；按文件条目算 30 → 26 只减成立（⑤ 门与语料回归走
   `.class` 计数不受影响）。
5. **死池残字**：16 类压池被封闭集拒绝（非零长 Raw 属性 → `AttrNotModeled`）
   保留死 Utf8 残字——依赖上游 cp_compact 扩面（libjbc 挂账方向），注解条目
   判据 residual 已清零（实现说明见 `src/grunt/metadata_strip.cpp` 文件头注释）。

## 3. 通用链取证与普查修复（通用链接入前置条件已解除）

K4 取证（修复前，代码基线 `6b38297`）：组合实测（专链三 pass + 通用主线 14
pass 单轮，产物 412,699 B，sha256 `cd99e539b771f06454c7d1b8c3a2b9f39a390877ac49aa3cfadd6044bf36a872`）静态门全绿（CLI rc=0；类数 25 → 23，DeadMember 摘 2 空壳类；VerifyJar `classes=23 ok=23 failed=0`；体积 -22%；通用面数字：蹦床 2057 / 代理内联 92+183 / 常量调用 29 / 伪标志 248 / 死方法 767），但功能运行 `NoSuchMethodError: t.aa(int,int,int)`——DeadMember 把 `t` 类 93 个方法全删（93 → 0），调用方 `w` 池里 `t.aa (III)I` Methodref（cp#634）仍在、`w.a` 运行期仍调用。

### 3.1 归因（普查漏计的通道，代码级）

DeadMember rewrite 的活引用普查把**压缩失败的类整体排除在引用方之外**：步骤 (1) 全容器压池时，cp_compact 封闭集拒绝的类（非零长 Raw 属性 → `AttrNotModeled`，本专链产物 16/25 类，§2-5 同源）落 `compactFailed`；步骤 (2) 普查对 `compactFailed` 类直接 return。于是 `w`（Raw 属性类）池里对 `t.aa` 的 Methodref 从全容器活引用集蒸发，`t` 的候选全部零票通过被删。`ref-rejected=0` 与 `compacted=0` 同帧出现是此通道的指纹。这不是 Kasumi 特有缺陷：K4 的通用链单跑（原始罐，generic-only）同样以 `NoSuchMethodError: t.aa` 告终（run-lab `out-kasumi-k4/run.out.err` 存证）——K4 记录的「原始罐同 pass 只删 57」里含 28 个同样漏计证据的误删，只是当时被 BSM 未收敛门部分掩盖（synthetic+bridge 整档宁留）。

### 3.2 修复（代码基线 `f9b25ce` 之上，本批落账）

普查改按**全容器对外引用全集**口径（宁误留不误删）：压缩成功的类枚举压缩后的池（闭包即活引用）；压缩失败 / 会话打不开的类按原始字节骨架解析（`readClass`，不解属性体，Raw 面不挡路）后枚举原池——未压缩池含死条目，枚举结果是活引用超集，正是宁误留口径；连骨架都解析不了的类置 census-blind，本轮成员删除与空壳回收整体放弃（引用证据完全不可得时没有成员能被证明死）。普查挂起时间片跑全量（proof 类数门同门），不许超墙截短出漏计。同病同修两处（同一「普查漏计 → 删动词放行」病形）：`field_census::scanFieldAccess`（消费方 ConstValuePromote 唯一写入门 / SeedStrip 写全在 clinit 门）补完整性台账，不完整整轮放弃；`booger_anno_strip::boogerReferenced`（@Booger 引用门）会话打不开的类按引用在册关死。DeadCode 无跨类普查（纯方法内控制流可达性），不同病。

### 3.3 修复后复验（数字，产物在 run-lab `out-dmfix/`）

- **DM 单跑（专链产物）**：scan 候选 875 不变；删除 767 → 426，`ref-rejected` 0 → 341，空壳 2 不变（a / i 两类照常摘）；`census-raw=0 census-blind=0`。`t` 类 93 → 0（修复前）变为 93 → **54**（`aa` 保留，恰为普查保护集），`w` cp#634 Methodref 原样在册。
- **JVM 实调拒证**：反射调 `w.a()` 零参双胞胎——修复前 `NoSuchMethodError: 'int dev.kasumi.obfuscator.t.aa(int, int, int)'`，修复后两个都正常返回（null / 0）。
- **组合链（专链 + 通用 14 pass 单轮）**：DM 段同数字；VerifyJar `classes=23 ok=23 failed=0`；`java -cp` 功能运行跑到超时（rc=124，stderr 干净）——K4 的 NoSuchMethodError 消失，且 §2-1 的 `h.c` VerifyError 在组合产物上未触发（通用链改写恰好绕开该帧面；§2-1 挂账维持不变）。
- **通用链单跑（原始罐对照）**：DM 候选 76 不变；删除 57 → 29（= 57 − 28，28 个即 §3.1 所指漏计误删，现被引用门拦下）；功能运行跑到超时——通用链单跑在原始罐上的同款 NoSuchMethodError 一并消失。
- **unittest**：158/158（155 基线 + 拒证 3 态：压缩失败引用方 / pool-only Methodref / census-blind 整轮停删，修复前 0/3、修复后 3/3）；corpus-regress 通用链与专链双跑 ok、失败 0。

## 4. 幂等与复跑

- 专链产物复跑专链：indy `total=0 sites=0`、字符串 `sites=0`（残 3 个
  noPlaintext 形状如实重报）、元数据三面全 0——幂等。
- 通用链收敛复跑（仅取证用）：第 2 轮剩 3 个蹦床、第 3 轮全零，产物字节
  与第 2 轮一致。
