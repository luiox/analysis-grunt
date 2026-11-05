# Kasumi 端到端复现记录（SnakeGame-obf）

> 样本：`SnakeGame-obf.jar`（run-lab 语料位；= morpher 树版样本
> SnakeGame-obf (1).jar，来源与 jar 账本见 [indy-decode.impl.md](indy-decode.impl.md)），
> 582,974 B，sha256 `c3481fb16f66ea9fade86e601a73b651db984666de18fe356bb439cc874f77fc`。
> 专链：`grunt.IndyDecode,grunt.StringCacheFold,grunt.MetadataStrip`（MetadataStrip 收尾，
> 先还原再清理）。jar / TSV / 产物一律不进仓，本文只记数字与来源。
> 对位：morpher 树版 `mjt-deobf/test/kasumi/verify.md`（origin/main）。
> 记录批次：K4 收官（代码基线 `6b38297`，unittest 155/155）。

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
   判据 residual 已清零，见 [metadata-strip.impl.md](metadata-strip.impl.md)。

## 3. 通用链取证（结论：不入专链）

组合实测（专链三 pass + 通用主线 14 pass 单轮，产物 412,699 B，sha256
`cd99e539b771f06454c7d1b8c3a2b9f39a390877ac49aa3cfadd6044bf36a872`）：

- **静态门全绿**：CLI rc=0；类数 25 → 23（DeadMember 摘 2 空壳类）；VerifyJar
  `classes=23 ok=23 failed=0`；体积 -22%（529,743 → 412,699 B）；通用面数字
  （蹦床 2057 / 代理内联 92+183 / 常量调用 29 / 伪标志 248 / 死方法 767）。
- **功能运行失败**：`NoSuchMethodError: t.aa(int,int,int)`——DeadMember 把
  `t` 类 93 个方法全删（93 → 0），而调用方 `w` 池里 `t.aa (III)I` Methodref
  （cp#634）仍在、`w.a` 运行期仍调用。单跑定位：`grunt.DeadMember` 单独作用
  于专链产物即复现（t 方法清零）；原始罐上同 pass 只删 57 个方法（原始罐
  BSM 未收敛，public synthetic+bridge 候选整档宁留，README 已载）。
- **归因**：引用普查漏计 Kasumi 改写类的对外引用（`w` 的池 / 指令面里对
  `t.aa` 的明文引用在普查中缺席）——普查面与 mjbase 改写产物的兼容性缺陷，
  与 §2-1 同属「改写产物 × 下游 pass」组合面，静态门照不到（类数不增 +
  VerifyJar 懒链接双盲区）。
- **判定**：增益真实但建立在误删之上，修复前**不入** Kasumi 专链；专链终态
  保持三 pass（链序 = K3 交接）。修复挂账：DeadMember 引用普查对改写产物
  的引用收集（或普查改池级保守口径）。

## 4. 幂等与复跑

- 专链产物复跑专链：indy `total=0 sites=0`、字符串 `sites=0`（残 3 个
  noPlaintext 形状如实重报）、元数据三面全 0——幂等。
- 通用链收敛复跑（仅取证用）：第 2 轮剩 3 个蹦床、第 3 轮全零，产物字节
  与第 2 轮一致。
