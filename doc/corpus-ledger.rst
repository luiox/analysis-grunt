.. _corpus-ledger:

语料账本（grunt 罐 corpus ledger）
========================================

本账本登记 grunt 家族语料罐的全账：罐产物路径 + sha256 + 类数（罐不进仓，产物留 Grunt 仓 ``samples/``，仓内只登记）、config 关键字段、基线站点量表、生成命令全录、摸底方法注记。
行为判据与缺口状态以 :ref:`obf-behaviors` 为准，residue 逐面文档在 ``doc/`` 根目录。
摸底用的普查脚本是一次性工具（``mjt/tmp_corpus/census_a.py``、\ ``census_b.py``），不进本仓；本篇记录其口径与方法使数字可复现。

.. _corpus-ledger-a:

1 罐 A——boar-main-v3-full-nonative（v3 Grunteon 全链 · native 关闭 · 前任批产出本批复验）
------------------------------------------------------------------------------------------

- **产物**：\ ``D:/WorkSpace/Grunt/samples/boar-main-v3-full-nonative.jar``，sha256 ``45357cadd07ddddc769810003aa8f4e105b267013a8e713bac862d8d84ca9f93``，3074 类 / 148044 方法（mjt-mcp project_open 指纹一致），24.3 MB。
  生成于 grunt3 分支 Grunteon（``e41e852``，wave2c 构建配置 diff 复用），输入 ``boar-main-origin.jar``\ （775 类，sha256 ``bd43b5cbf607b67c2fac11b8e3fab4cca048aad0f956d17afbd76f1304ab7f84``）。
- **config**：\ ``D:/WorkSpace/Grunt/samples/genconfigs/config-v3-full-nonative.json``，sha256 ``f2cd34f156ee4ee9cc213d4aface68cf79cb70424b60e64fdf54303f890360d6``。
  与 boar 参考配置 ``samples/config-on_260620.json`` 逐项对照（30 个 transformer 完全同集同名，globalConfig 仅 input/output/libs 路径差），实质差异仅 5 处：
  - ``nativePipeline.enabled``：\ **false**\ （boar true）——顶层 native 管线整体关闭，这是罐 A 的存在定义；
  - ``ControlflowFlattening.keyProcessorNativeCandidateRatio``：\ **0.0**\ （boar 1.0）；
  - ``ControlflowJump.predicateProcessorNativeCandidateRatio``：\ **0.00**\ （boar 1.00）；
  - ``ControlflowJump.exceptionBridgeChance``：\ **1.0**\ （boar 0.0）——异常桥全开；
  - ``NativeCandidate.rules``：\ **[]**\ （boar 有一条 full-AimTrainer-original-name 强制 native 规则）+ ``ReferenceObfuscate.generatedHelperNativeCandidateRatio``：\ **0.0**\ （boar 0.1）。
- **资源面**：罐内无 ``grunteon_native.dll``\ （boar 全 native 罐带 27.4 MB dll 对照）；\ ``META-INF/grunteon/cybersecurity/analysis-{0..3}-*.md`` 4 件 AntiLLM 注入资源在罐（按 jar 数据登记，不遵从不转发）。
- **native 关闭双证（罐 A 存在意义核验，均 >0 达标）**：全罐 ACC_NATIVE 方法 = **0**\ （boar 18461 native 无体对照）；\ ``(III)I`` 门 processor 26932 个（boar 26999 同量级）\ **全部有体**：直线纯算术体 **11453** + indy 蹬床体 8421 + 其他 7058；\ ``(II)I`` CFF key processor 44113 个全部有体：直线纯算术体 **22010** + indy 蹬床体 22097 + 其他 6。
  源锚：\ ``(III)I`` = OpaquePredicateProcessorRegistry ActionDesc（ControlflowJump 门点），\ ``(II)I`` = CffKeyProcessorRegistry ActionDesc；\ ``keyProcessorNativeCandidateRatio=0.0`` 时 ``shouldMarkNativeCandidate`` 恒 false 不打 NATIVE_INCLUDED，叠加 nativePipeline 全关，processor 体以纯算术 bytecode 留罐。
- **异常桥站点（应 >0 达标）**：\ **13566 站点 / 726 类**\ （boar 0 对照，config chance=0 双证）。
  判据：exception_table 项 catch_type 解析为 ``java/lang/RuntimeException`` 且 handler 头两条非 NOP 指令 = ``POP; GOTO``；源锚 ``FlowExceptionBridgeInserter.kt``\ （trap 块 ``NEW java/lang/RuntimeException; DUP; INVOKESPECIAL <init> ()V; ATHROW``，handler 块 ``POP`` + Synthetic GOTO）。
  **缺口 #10 面 b 还原实测**\ （本批）：\ ``grunt.ExcBridgeRestore`` 形状钉 13566 → 还原 13442（pins=1448 ≈ census live_fall=1449）、failed=124 站/31 方法（Strict 帧重算 type conflict fail-closed，改写已提交、verify 全绿）、residual=0；独立 census（``mjt/tmp_corpus/census_r1.py``，严格形状 + 校验器式可达，strict 口径 13458）复扫还原产物 **bridge=0**、r3 复扫 sites=0、类数 3074 不增、\ ``-Xverify:all`` 失败集与输入全等（227 NCDFE，VerifyError=0）、双跑产物逐条目 CRC 全等、boar 对照罐 no-op（类条目 CRC 逐条全等）；链序实证 ``…ExcBridgeRestore,CffUnflatten`` 岛还原 1281→1543。
- **供给链站点量**\ （grunt_cli，\ ``GRUNT_TRACE=1``，\ ``--jdk C:/Users/Canrad/.jdks/corretto-17.0.5``）：

  .. list-table::
     :header-rows: 1
     :widths: 30 42 28

     * - pass
       - 罐 A（本批实测）
       - boar 台账对照
     * - IndyRestore scan
       - total=77671 sites=31196 rejects=46475（key=46475 malformed/decode/desc/magic/metadata/**material=0**/iface=0）
       - #1 sites=30924，materialLane 5060 残
     * - IndyRestore rewrite
       - classes=724 sites=31196 failed=0（static=30120 virtual=1076）proof=ok
       - 30924
     * - StringPoolFold
       - pool-classes=724 triple-hits=19610 → applied=19512 failed=98 proof=ok
       - 19244
     * - ConstantCall
       - sites=**24479** applied=24479/24479（classes=724）proof=ok（``GRUNT_SLICE_SEC=0`` 全量）
       - 解锁链 7→687
     * - CffUnflatten（链 ``IndyRestore,StringPoolFold,CffUnflatten``）
       - dispatchers=9886 islands-solvable=1226 rejects=8660（key-processor=5100 foreign-ref=487 shape-mismatch=3073 其余桶 0）；rewrite islands=1201 scrambles=260 entry-restored=140 entry-trampoline=196 failed=25 proof=ok
       - 9880 岛还原 1481
     * - CffUnflatten（解锁链 ``IndyRestore,ConstantCall,CffUnflatten``，\ ``GRUNT_SLICE_SEC=0``）
       - islands-solvable=1307 key-processor=5013 islands=1281 failed=26 proof=ok
       - 桶 6201→6121、islands 1547
     * - ExcBridgeRestore（缺口 #10 面 b，\ ``GRUNT_TRACE=1`` ``GRUNT_SLICE_SEC=0``）
       - scan sites=13566 classes=726 rejects=4702（shape=4702 其余桶 0）；rewrite classes=726 methods=4451 bridges=13442 pins=1448 insns=80652 failed=124（31 方法 Strict 帧重算 type conflict，改写已随 lower 提交）proof=ok residual=0
       - —
     * - CffUnflatten（链 ``IndyRestore,StringPoolFold,ConstantCall,ExcBridgeRestore,CffUnflatten``）
       - islands-solvable=1573 key-processor=6155 foreign-ref=292 shape-mismatch=1866；rewrite islands=1543 failed=30 proof=ok residual=30
       - 无本 pass 链 islands 1281 → **1543（+262）**
     * - GateProcessorFold（缺口 #10 面 a，原罐单跑 ``GRUNT_TRACE=1`` ``GRUNT_SLICE_SEC=0``）
       - scan shape=13782 rejects=10983（args=104 forward=2379 tlr=2361 eval-bad=6139 not-true=0 其余桶 0）；rewrite classes=721 sites=**7643/7643** failed=0 proof=ok residual=0；类数 3074 不增；\ ``-Xverify:all`` 失败集与输入全等（227 NCDFE，VerifyError=0）
       - 无上游依赖（ReferenceObfuscate 跳过 @GENERATED_METHOD，门点调用从不被 indy 包装）
     * - GateProcessorFold（组合链 ``IndyRestore,StringPoolFold,ConstantCall,ExcBridgeRestore,GateProcessorFold,CffUnflatten``）
       - scan shape=7968 rejects=9374（args=3479 forward=1119 tlr=1090 eval-bad=3686 not-true=0 其余桶 0）；rewrite classes=720 sites=**4282/4282** failed=0 roundtrip-skip=0 proof=ok residual=0；链上四上游 pass 与 CffUnflatten islands=1543 逐项同锚链；双跑产物逐条目 CRC 全等
       - census 对拍（r1 供给链口径）：shape=7968 / 折叠 4282 / not-true=0 逐项同源；boar 对照罐单跑 sites=0 no-op（类条目 CRC 逐条全等）
     * - NumberFold（缺口 #8 前·旧四式，本批复验）
       - scan sites=22939；rewrite classes=726 sites=21525/22939 failed=1414 proof=ok
       - —
     * - NumberFold（缺口 #8 后·六式，含 v2 位拼两式）
       - scan sites=43060 = 旧式 22939 + 位拼 20121（加性恒等，旧式站点数逐位不动）；rewrite classes=1447 sites=41596/43060 failed=1464 proof=ok；旧版产物上纯位拼面 scan=20121 → 20071/20121 failed=50 proof=ok（21525+20071=41596 精确）；双跑逐条目 CRC 全等（3088 条目同序零差异）；\ ``-Xverify:all -Xmx1g`` 失败集与输入全等（2847 ok / 227 NCDFE）
       - —

- **key-processor 桶消化机制注记（罐 A 上未大幅消化，487→5013 量的原因已钉）**：解锁链只吞 87（5100→5013），与 boar 的 66 同量级——CffUnflatten 的 processor span 形判据（``emitProcessorStateProgram`` 逐字：\ ``INT inputKey; ISTORE s; ILOAD s; INT salt; INVOKESTATIC proc (II)I; ISTORE s``）里第一参走 ``ILOAD s``，ConstantCall 的「参数全常量」站点门吃不下该形；CffUnflatten 本身按设计不跨方法内联求值 processor 体（缺口 #10 面 (a) 范围）。
  罐 A 的贡献在\ **燃料侧**：22010+11453 个直线纯算术体在手（boar 是 0），跨方法求值实现后即可整桶消化——重启条件从「语料存在性」升级为「纯实现工作」。
  **面 (a) 已由 ``grunt.GateProcessorFold`` 闭环**\ （ControlflowJump (III)I 门点；上表两行实测；(II)I CFF key processor 的 span 形仍在缺口 #4 余量，span 求值第一参 slot-fed 不在本 pass 面，规格见 :ref:`gate-processor-fold-spec`）。
- **门点普查工具注记**：缺口 #10 面 a 取证用 ``mjt/tmp_corpus/census_gate.py``\ （罐 A 原罐与 r1 供给链产物双口径：(III)I invokestatic 全量 → 逆向三常量 + 前窗 ``[INT; (INT; OP)*]`` 走廊 + IF_ICMP 钉形 → 门点 8747（plain 7968 + tlr 779）→ 目标链三级解 → proc 体 Python 求值 + 恒真对拍）；r1 口径关键量 raw=13656 / const-fed=10177 / plain-foldable=4282 / verdict-true=4282。
  普查脚本一次性工具不进仓，判据已逐条录入；TLR 体 110 在普查为 eval-success+verdict-false，实现口径正确落 EvalBadState（普查求值器对 TLR 面过宽，以 pass 为准）。
- **可加载性（-Xverify:all 全类加载，自写驱动逐类 ``Class.forName(n, false, cl)``）**：2847/3074 加载成功，\ **VerifyError/ClassFormatError = 0**；227 个失败全为 NoClassDefFoundError 缺外部依赖（kotlin.jvm.functions.Function0~5 共 209、kotlin.properties 14、kotlinx.coroutines 3、org.lwjgl.opengl.GL46 1）——依赖缺失非罐缺陷。
  javap ``-c`` 全量 dump 在 junk 类上崩溃（``IllegalStateException``，DecompilerCrasher 面），字节码普查一律走自写解析器。
- **类数增长面**：775 → 3074（+2299）。
  罐内类名经 ClassRenamer 全量重命名，与 origin 类名零重合（名字对照无意义，如实注记）；非 obf 包新增仅 ``net.spartanb312.grunteon.internal.Metadata`` 1 类（indy heavy 面 ``@Metadata`` 载体）；\ ``$KeyProcessor$`` 伴生类名同样被重命名吞掉（名字形状普查在改名后罐上无效，改用 desc + 体形普查）。
- **生成命令全录**：罐 A 生成命令为前任批遗产（grunt3 分支 Grunteon ``writeClasspath`` + config-v3-full-nonative.json，工作区 diff 见 Grunt 仓 buildSrc/grunt-main build.gradle.kts）；本批复验命令：\ ``grunt_cli -i D:/WorkSpace/Grunt/samples/boar-main-v3-full-nonative.jar -o <out> -p grunt.IndyRestore,grunt.StringPoolFold,grunt.ConstantCall --jdk C:/Users/Canrad/.jdks/corretto-17.0.5``\ （``GRUNT_TRACE=1``、全量加 ``GRUNT_SLICE_SEC=0``）；CFF 链同形换 ``-p grunt.IndyRestore,grunt.StringPoolFold,grunt.CffUnflatten`` 与 ``-p grunt.IndyRestore,grunt.ConstantCall,grunt.CffUnflatten``。

.. _corpus-ledger-b:

2 罐 B——boar-main-v2-full（v2 Gruntpocalypse 2.5.0 全链 · 本批产出）
----------------------------------------------------------------------

- **产物**：\ ``D:/WorkSpace/Grunt/samples/boar-main-v2-full.jar``，sha256 ``b1c471593979c32469473a622f92b6cc5e4b022e87d9e58d4fb6fd70f6c51b73``，\ **10486 类** / 142423 方法（+17 方法解码失败 = Crasher junk 畸形体，如实计），104 MB。
  输入同 ``boar-main-origin.jar``，与罐 A 同输入兄弟罐。
- **构建方式**\ （grunt2 分支 ``36c0eef``，tag 线 2.0.0~2.4.5 后的 2.5.0 build 250415）：\ ``git worktree add /d/WorkSpace/grunt2-build grunt2``\ （不混 grunt3 工作区 diff）；Gradle 8.14 wrapper（本机已有 dist 缓存）+ ``GRADLE_OPTS="-DsocksProxyHost=127.0.0.1 -DsocksProxyPort=10809"`` 拉依赖（fastutil 直连超时后走代理成功）；\ ``./gradlew :grunt-main:fatJar --no-daemon`` 产出 ``grunt-main/build/libs/grunt-main-full.jar``\ （Main-Class ``net.spartanb312.grunt.GruntKt``）。
  运行需 **JDK 21**\ （v2 代码调 ``List.removeLast()``，Java 21 SequencedCollection API，JDK 17 上 NoSuchMethodError；用 corretto-21.0.4）。
- **config**：\ ``D:/WorkSpace/Grunt/samples/genconfigs/config-v2-full.json``——默认 config（工具首跑自动生成）改 Settings.Input/Output 指向 origin/产物 + 除 PostProcess 外 34 个 transformer ``Enabled=true``，其余参数全默认（NumberEncrypt Intensity=1、ArithmeticEncrypt Percentage=50、ConstBuilder ReplacePercentage=10/MaxCases=5、InvokeDynamic ReplacePercentage=10、\ **TrashClass/ClonedClass Count=0 默认不生成**——罐 B 无垃圾类增长面，类数 775→10486 的增长来自 ConstPoolEncrypt 伴生类与 scramble 结构面）。
- **运行日志关键量**\ （v2 工具自带 per-transformer 日志，676 s）：SourceDebugRemove 22562；KotlinOptimize 1564 Intrinsics + 982 注解；EnumOptimize 33；DeadCodeRemove 2736；MethodExtractor 3113；HWID 1327 校验 / 5 cache pools；DeclareFields 20；ReflectionSupport 1330；\ **StringEncrypt 计数 0（假象，见下）**；NumberEncrypt **155154**；ArithmeticEncrypt **50700**；Controlflow 24243 jumps；\ **ConstBuilder 0（真 0，见下）**；ConstPoolEncrypt **804168 constants / 5243 pools**；StringEqualsRedirect 1；FieldScramble 3850；MethodScramble 14267；NativeCandidate 69 注解；SyntheticBridge **1929856**；LocalVariableRename 25691；MethodRename 110172（后续 rename 族略）；运行尾 Missing-* ERROR 行为 libs missing-check 对 origin 外部依赖（lwjgl/assimp/oshi 等）的提示，非错误。
- **四 P2 目标形态站点量**\ （自写类文件解析器 census_b.py，指令级对齐解码，cp 索引解析 Fieldref/NameAndType；口径注记随条目）：

  - **#5 ConstPoolEncrypt 伴生类面**：形状普查 **2516 伴生类 / 947181 个 static ConstantValue 数值字段 / 938409 个 GETSTATIC 调用点**\ （判据：≥10 个 static ConstantValue 数值字段的类；调用点 = GETSTATIC owner ∈ 该集合；日志权威数 5243 pools / 804168 constants 与普查同量级，差额 = 水印/伴生自读计入与集合口径）。
    解密器形状锚（static ``(Ljava/lang/String;)Ljava/lang/String;`` + 体含 IXOR/I2C）普查 0——伴生类 clinit 与解密器体被二次过 NumberEncrypt/StringEncrypt（源码 173-174/187-188 行），锚失效是机制不是缺失。
    mapping dump（226 MB，classes/fields/methods.json）在 ``mjt/tmp_corpus/mappings/2026-11-07 08-35-06 boar-main-origin.jar/``，回填批的旁证库。
  - **#6 ConstBuilder switch 面**：\ **0**\ （日志 "Generated 0 number builders" 为真 0）——机制：order 301 NumberEncrypt 先把 int 常量全走廊化，order 500 ConstBuilder 无米下锅（默认管线序的饿死面）；全罐 switch 总面 121236（其中 104263 前窗含 GETSTATIC+IXOR）属 Controlflow SwitchProtect/ChaosSwitch 面，不是 ConstBuilder 形。
    语料要出 #6 形需关 NumberEncrypt 或调序重产罐。
  - **#7 ArithmeticEncrypt 恒等面**：日志 50700 替换；输出罐中\ **精确输出 n-gram = 0**\ （九式输出形全含 ``ICONST_M1``/``ICONST_1`` 字面常量，被 order 501 ConstPoolEncrypt 80 万级常量改写打散邻近性——AE 面在此罐整体 compost 进 #5 面）；幸存 AE 家族对齐形：TRIP1 ``(ICONST_M1; IXOR; IAND)`` 41421、TRIP2 ``(ISUB; ICONST_M1; IXOR)`` 2（ArithmeticExprBuilder junk 同族形）。
    **还原面实际口径**\ （简化链按 const-fed 吃，不分来源）：const-fed IXOR **410536**、IOR **9786**、IAND 212222 + 25372（GETSTATIC 喂给）。
  - **#8 NumberEncrypt IAND/LAND 位拼面**：const-fed（双操作数均字面常量推送）\ **IAND 212222 / LAND 141868**；GETSTATIC 喂给变体 **IAND 25372 / LAND 32718**；long 走廊形（``int;I2L;int;I2L;LXOR``）\ **212122**\ （日志 155154 加密数，差额 = junk builder 同形混入）。
    **组成注记**：const-fed 口径是 NumberEncrypt 位拼模板与 Controlflow ArithmeticExprBuilder junk 的混合人群，还原模板（first & second = v 折回）按形状吃、不分来源，此混合口径即还原面站点量。

    - **本批复算对账（缺口 #8 落地批，全量解码 + ConstantValue 跨类注册表口径）**：四锚精确复现——类 10486 / 方法 142423 / long 走廊 trailing-L2I 212122 / const-fed IOR 9786；const-fed IAND 复算 **211697**\ （台账 212222，差 −525）、LAND 复算 **141873**\ （台账 141868，差 +5）——差额与 junk 区解码重同步窗口同量级，结论不变；GETSTATIC 喂给桶原一次性普查脚本已删、判据不可复现，本批矩阵记 ``IAND('gs',*)=45794`` / LAND=13287，与台账 25372/32718 差异记为\ **口径差**\ （双方均不采信为权威数，待重产罐重测）；gs 喂给位拼在缺口 #5 落地前 fail-closed 不收，余量面见下条还原实测。
    - **还原实测**\ （``grunt_cli -i boar-main-v2-full.jar -p grunt.NumberFold`` 单 pass 单进程，\ ``GRUNT_SLICE_SEC=0``）：scan **777970**\ （旧四式 423880 + 位拼 354090，旧式数与旧版二进制 scan 逐位一致）；rewrite classes=**5398** sites=**777970/777970** failed=0 proof=ok；残留 const-fed IAND 211697→**2**、LAND 141873→**5**\ （折掉 **141868** = 台账 LAND 精确）、走廊 212122→0、gs 喂给面原样不动；类数 10486 不增；\ ``-Xverify:all -Xmx1g`` 全类加载失败集与输入逐行全等（10259 ok / 227 NCDFE）；双跑逐条目 CRC 全等（10492 条目同序零差异）。
    - **跑法注记**：NumberFold 罐 B 全程 >60s，默认 ``GRUNT_SLICE_SEC``\ （60s 时间片）会把 rewrite 截断——截断跑 proof 仍 ok、产物条目 CRC 与全量跑差 4561 条（本批实测），非确定性缺陷，是 pass_clock 既有防超时语义；罐级对账一律 ``GRUNT_SLICE_SEC=0`` 全量跑。
    - **内存实测**：罐 B 单 pass 峰值工作集 **6386MB**\ （scan 后 ~3.6GB + FrameContext 全容器会话常驻 +~2.7GB + pass 本体 ~0.1GB；commit 峰值 9044MB）；罐 A 同 pass 峰值 986MB（3074 类）——类数→内存近线性（罐 B 类更肥，伴生类均数百 ConstantValue 字段）；本批 pass 内瘦身（站点去字符串化 / 类字节流式就地回写 / 聚类即弃）已在白名单内落地；大头为既有基建（rewrite_toolbox FrameContext 会话生命周期 + scan 期堆高水驻留），登跨批性能账另行处理；bad_alloc 触发机制为系统 commit 头寸耗尽（页面文件无法扩容时），C 盘满会锁死页面文件扩容上限——跑罐 B 前查 commit 头寸与 C 盘余量。
  - **关联面（#1 字符串池）**：v2 池三件套在罐：\ **1333 池类 / 1339 个 ``String[]`` static 字段 / 3910 个 ``([CJI)Ljava/lang/String;`` 解密器**——StringEncrypt 日志 "Encrypted 0 strings" 是 v2.5.0 计数器假象（池结构实存；调用点 ``GETSTATIC; idx; AALOAD`` 三元组普查 0 因 idx 被 order 301 NumberEncrypt 走廊化，非池缺失）。
- **基础面**：invokedynamic 站点 **37227**\ （raw 罐内）；ACC_NATIVE 0（v2 无 native 管线）；Watermark ``NAME()`` 返回 'PROTECTED BY GRUNT KLASS MASTER' 形 887 类；全罐无 AntiLLM 资源（v2 无该面）。
- **生成命令全录**：\ ``git worktree add /d/WorkSpace/grunt2-build grunt2`` → ``GRADLE_OPTS="-DsocksProxyHost=127.0.0.1 -DsocksProxyPort=10809" ./gradlew :grunt-main:fatJar --no-daemon``\ （cwd /d/WorkSpace/grunt2-build）→ 首跑 ``corretto-21.0.4/bin/java -jar grunt-main-full.jar <cfg>`` 生成默认 config → 改写为 config-v2-full.json → ``corretto-21.0.4/bin/java -Xmx8g -jar grunt-main-full.jar D:/WorkSpace/mjt/tmp_corpus/v2_full_config.json``。

.. _corpus-ledger-method:

3 方法注记（复现口径）
----------------------

- **普查工具**：\ ``mjt/tmp_corpus/census_a.py``\ （罐 A，仅解码 ``(III)I``/``(II)I`` 方法体——生成面 MAXS(8,2) 全用隐式槽指令形，全罐零解码失败即此口径健全性的旁证；异常桥按 exception_table 原始字节判）、\ ``census_b.py``\ （罐 B，全方法解码 + cp Fieldref/NameAndType 解析；17 方法 Crasher junk 解码失败如实计并排除在算术面分母外）、\ ``census_r1.py``\ （罐 A 供给链产物 r1/r3 复扫，严格形状桥 + 校验器式可达普查——SMT 帧根 + 异常表 handler 根 BFS，供缺口 #10 面 b 还原前后对照）、\ ``mjt/tmp_corpus8/census8d.py``\ （缺口 #8 复算批：罐 B 全方法解码 + cp Fieldref 解析 + 跨类 ConstantValue 注册表（GETSTATIC→伴生字段带 ConstantValue 计 const-fed 'cv'），NOP 夹层邻接窗 + junk 区重同步容差；const-fed 与走廊锚四数见 §2 #8）。
  四脚本一次性工具不进仓，判据已逐条录入上文。
- **CLI 统计开关**：\ ``GRUNT_TRACE=1``\ （per-pass scan/rewrite/rejects 桶）；\ ``GRUNT_SLICE_SEC=0``\ （关 60 s 时间片——ConstantCall 首跑 slice 截断在 oo.class 且 proof FAILED 是截断伪影，全量复跑 24479/24479 proof=ok；罐 A 供给链最终数以全量跑为准）。
- **javap 边界**：\ ``javap -c -p`` 全量 dump 在两罐的 junk/畸形类上崩或挂死（DecompilerCrasher 面），一切字节码普查禁止依赖 javap 全量跑；单类抽样需挑小类。
- **mjt-mcp**：罐 A 曾以临时 project 打开（指纹 ``sha256:45357cad…`` 与 sha256sum 一致交叉验证）后关闭，无编辑。
- **boar 台账对照数字来源**：\ :ref:`obf-behaviors`\ §5（缺口清单：IndyRestore#1 30924 / StringPoolFold 19244 / materialLane 5060 / key-processor 6201→6121 / islands 1481→1547 / (III)I 26999 = 18461 native + 8538 蹬床 / 异常桥 0）。
