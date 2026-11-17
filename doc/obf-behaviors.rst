.. _obf-behaviors:

grunt 行为记录总表
========================

.. _obf-behaviors-scope:

1 定位
------------

本篇是 analysis-grunt 的 **grunt 混淆行为记录总表** ：把 Grunt 家族两代（v2 Gruntpocalypse / v3 Grunteon）的每个混淆行为逐个写成做什么（字节码形态 + 可汇编 jasm 样例）/ 配置面 / 判据分级 / 本仓 residue 现状，并汇总行为 × 处理状态的总表与遗留缺口清单，供后续还原排期。逐行为细节（形态 + 配置面 + jasm 样例）在两篇分篇（§2），本篇承载总表与缺口清单。

.. _obf-behaviors-index:

2 文档索引
------------

- :doc:`obf-behaviors/grunt-v2-behaviors`\ ，v2（ ``grunt2`` 分支 ``36c0eef`` ，tag 线 2.0.0 ~ 2.4.5）35 个 transformer 行为记录 + 16 个已验证 jasm 样例。
- :doc:`obf-behaviors/grunt-v3-behaviors`\ ，v3（ ``grunt3`` 分支 ``bda0b1a`` ，Grunteon）39 个 transformer + GlslObfuscator 插件 + NativePipeline 阶段的行为记录 + 11 个已验证 jasm 样例 + 增量提交并入注记 + 产物面全局配置登记。
- jasm 样例验证口径与已知校验器边界（ ``xaload`` 族误报、\ ``<init>`` 栈帧分析缺陷）见 v2 分篇 §0.3：\ **27/27 通过（25 relaxed + 2 off 通道）** 。
- Kasumi 家族专面在 ``doc/kasumi-verify.rst``\ ，不属本篇。
- residue 逐面文档（number-fold / proxy-inline / predicate-fold / …）在 ``doc/`` 根目录，本篇的状态列以它们为事实源。
- 复杂 pass 规格篇已起步：\ :ref:`gate-processor-fold-spec`\ （全家族复杂 pass 文档的样板篇）。

.. _obf-behaviors-v2:

3 v2 行为 × 处理状态总表（32 行覆盖 35 transformer，Kr 变体并行）
----------------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 22 10 12 56

   * - 行为
     - 等级
     - 状态
     - residue / 缺口
   * - DeclareFields
     - S1
     - 已处理
     - ConstValuePromote（判据互为镜像）
   * - FieldScramble
     - S2
     - 已处理
     - FieldAccessProxy
   * - MethodScramble
     - S2
     - 已处理
     - ProxyInline
   * - SyntheticBridge
     - S2
     - 已处理
     - FlagsStrip 摘 static 成员上的伪标志；实例成员的伪标志保守保留
   * - NumberEncrypt
     - S1
     - 部分
     - NumberFold（IAND/LAND 位拼两式已扩，见缺口 #8）
   * - ConstPoolEncrypt
     - S1
     - 已处理
     - ConstPoolRestore 调用点回填 + NumberFold/ConstValuePromote 供给链（缺口 #5 已处理完；伴生类本体不删交垃圾类面 #14，HeavyEncrypt 二次加密面逐类 fail-closed）
   * - Controlflow
     - S1/S2
     - 部分
     - TrampolineSqueeze / PredicateFold / NumberFold / DeadCode / SeedStrip 覆盖静态可裁决子集；垃圾调用残留与陷进环挂缺口 #16，跳转变异的直线算术形态挂缺口 #7
   * - InvokeDynamic
     - S1
     - 部分
     - simple 形已完整还原：indy 调用里加密的方法名能直接解出来换回原调用（对应缺口 #2）；heavy 形暂不处理：还缺认出混淆器版本的条件，硬解有改错程序的风险；另有 IndyFold 靠模拟执行做备用还原
   * - StringEncrypt
     - S1
     - 已处理
     - StringPoolFold（literal 形折叠；供给链被 indy 藏面时依赖上游还原，实现说明见 ``src/grunt/string_pool_fold.cpp`` 文件头注释）+ PoolArrayFold（池类 ``<clinit>`` 跨方法求值通道压池，``GETSTATIC; idx; AALOAD`` 读点折明文——q8 样本 C 池读 residual=0，见 :ref:`corpus-ledger`）
   * - ConstBuilder
     - S1
     - 无处理
     - 缺口 #6
   * - ArithmeticEncrypt
     - S2
     - 已处理
     - ArithFold（替换八式恒等折叠 + 常量窗整窗归一，v2 MutateJumps 直线形缺口 #18 同面）
   * - MethodExtractor
     - S2
     - 无处理
     - 缺口 #9
   * - StringEqualsRedirect
     - S2
     - 已处理
     - StringEqualsRestore（hashCode 比较桥换回 ``equals`` / ``equalsIgnoreCase``\ ，v2/v3 逐字同形一套形状表）
   * - Crasher
     - S2
     - 部分
     - JunkRegionStrip 只吃不可达无效码段（字节级可达性走查，长度保持 NOP 化）；可达段 junk 与 massive Signature 属性本体挂缺口 #13
   * - Watermark
     - S2
     - 部分
     - BoogerAnnoStrip 摘 @Booger 运行时注解（容器级引用普查门控，解密器体折叠后解封）；水印字段/方法本体挂缺口 #13
   * - ClonedClass / TrashClass
     - S2
     - 无处理
     - 缺口 #14
   * - HWIDAuthenticator
     - 检测-only
     - 无处理
     - 缺口 #13（检测/摘除面）
   * - NativeCandidate
     - 检测-only
     - 无处理
     - 登记即可（外部管线选取面）
   * - MethodRename(+Kr) / FieldRename(+Kr) / ClassRename / Mixin 改名
     - 清理面
     - 无处理
     - 改名还原走 mapping 旁证，不在 residue 面
   * - SourceDebugRemove / KotlinOptimize / EnumOptimize / DeadCodeRemove / LocalVariableRename / ShuffleMembers / Shrinking / PostProcess
     - 清理面
     - 清理面
     - 还原侧无动作
   * - ReflectionSupport
     - 伴随面
     - 伴随面
     - 产物无独立形态

.. _obf-behaviors-v3:

4 v3 行为 × 处理状态总表（39 transformer + 插件 + 阶段 + 产物面全局配置）
--------------------------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 22 10 14 54

   * - 行为
     - 等级
     - 状态
     - residue / 缺口
   * - NumberBasicEncrypt
     - S1
     - 已处理
     - NumberFold 全覆盖（四式全对齐）
   * - DeclaredFieldsExtract
     - S1
     - 已处理
     - ConstValuePromote
   * - InvokeProxy
     - S2
     - 已处理
     - ProxyInline
   * - FieldAccessProxy
     - S2
     - 已处理
     - FieldAccessProxy
   * - FakeSyntheticBridge
     - S2
     - 已处理
     - FlagsStrip
   * - ControlflowJump
     - S1/S2
     - 部分（异常桥 + processor 检查已处理完）
     - TrampolineSqueeze / PredicateFold / NumberFold / DeadCode / ExcBridgeRestore / GateProcessorFold（material 类 gate 转发求值不可解 → 缺口 #15 联动余量；TLR 运行期随机面永久排除）
   * - ReferenceObfuscate
     - S1
     - 部分
     - IndyRestore（缺口 #2 已处理完：simple 直接解密回填 + heavy ``@Metadata`` 核对回填，\ ``Era::Grunt3`` 检查；RuntimeMaterial 联动的材料通道由 MaterialKeyRestore 折回 key 后第二轮本 pass 消化；260705 带 ``while`` 的解密器三形键窗原按 dom.order() 位置回溯失配（keyMissing=984），收编 ``const_args::decryptKeyWindow`` pc 布局真值单源后 t3_full indy 2311→3，台账解锁批 ②）+ IndyFold 作模拟执行备用还原
   * - StringArrayedEncrypt
     - S1
     - 已处理
     - StringPoolFold（arrayed 形折叠；供给链被 indy 藏面时依赖上游还原，实现说明见 ``src/grunt/string_pool_fold.cpp`` 文件头注释）+ PoolArrayFold（池类求值通道压池，同 :ref:`v2 表 <obf-behaviors-v2>` StringEncrypt 行）
   * - InvokeDispatcher
     - S2
     - 已处理
     - DispatcherRestore（头锚 + 键表 + 调用点 key 反查回填；分发器本体与键表交 DeadCode/DeadMember 回收。宿主方法被 CFF 摊平或调用点被 ReferenceObfuscate indy 化时依赖链序/上游还原，实现说明见 ``src/grunt/dispatcher_restore.cpp`` 文件头注释）
   * - ControlflowFlattening（含 SSA exp）
     - S1/S2
     - 已处理（主面）
     - CffUnflatten（岛识别 + span 求值反摊平 + alias-face 剪枝 + 入口归位/handler 重绑/位置边钉，缺口 #4 主面已处理完，实现说明见 ``src/grunt/cff_unflatten.cpp`` 文件头注释；processor 键解锁（NumberFold 先行）+ SSA exp 变体 → 缺口 #4 余量）
   * - ArithmeticSubstitute（Hidden）
     - S2
     - 已处理
     - ArithFold（替换八式与 v2 逐字相同，同一形状表）
   * - StringEqualsOptimize
     - S2
     - 已处理
     - StringEqualsRestore（与 v2 逐字同形，一套形状表）
   * - ParameterObfuscate
     - S2
     - 无处理
     - 缺口 #12
   * - TrashClassGenerator
     - S2
     - 无处理
     - 缺口 #14
   * - RuntimeMaterial
     - S2
     - 部分（联动）
     - MaterialKeyRestore（缺口 #15 联动面已处理完：材料通道 key 清洁口径静态折回，\ ``parseUnsignedLong``/``fold`` 固定 JVM 语义直解不需模拟执行；indy_restore 第二轮消化 materialLane 桶，boar 专链 42228 → 5060；扰动摘除面仍挂缺口 #13，native materialKey 70 类余量）
   * - AntiLLM
     - S2
     - 无处理
     - 缺口 #13
   * - Watermark
     - S2
     - 无处理
     - 缺口 #13
   * - DecompilerCrasher
     - S2
     - 部分
     - JunkRegionStrip：不可达无效码段原位 NOP 化（字节级可达性走查 + 布局保持，SMT/LVT/LNT 零扰动，样本 C 实测死区字节 0 / fail-closed 0 / decode-fail 0）；Crasher 大属性本体挂缺口 #13
   * - NativePipeline
     - 检测-only
     - 无处理
     - 不可静态还原；识别 = native 方法 + loader 类 + 库资源计数（缺口 #15 检测项）
   * - NativeCandidate
     - 检测-only
     - 无处理
     - 登记即可（NativePipeline 选取面）
   * - ClassRenamer / MethodRenamer / FieldRenamer / Mixin 改名
     - 清理面
     - 无处理
     - 改名还原走 mapping 旁证
   * - MethodInliner / ClassShrink / KotlinClassShrink / DeadCodeRemove / EnumOptimize / SourceDebugInfoHide / LocalVarRenamer / ShuffleMembers / PostProcess
     - 清理面
     - 清理面
     - 还原侧无动作
   * - FlowIRRoundTrip / SSARoundTrip（Hidden）
     - 伴随面
     - 伴随面
     - IR 往返 infra，产物与输入等价
   * - NativePreProcessor
     - 伴随面
     - 伴随面
     - native 前置桥接（ ``__grt$indy$``/``__grt$condy$``/``__grt$mh$`` 形状可识别）
   * - ReflectionSupport
     - 伴随面
     - 伴随面
     - 产物无独立形态
   * - MappingApplier
     - 伴随面
     - 伴随面
     - 外部 mapping 应用
   * - GlslObfuscator（grunt-glsl 插件）
     - 资源面
     - 无处理
     - GLSL 文本改写，字节码 residue 不适用，登记
   * - corruptCRC32 / corruptHeaders（产物面全局配置）
     - 登记面
     - 无处理
     - 产物 zip 逐条目破坏 CRC32 或混淆 zip 头，反分析开关；本仓不处理，读取侧按需容错（v3 分篇 §4.5）

.. _obf-behaviors-gaps:

5 遗留缺口清单（按优先级，供后续还原排期）
--------------------------------------------

**P1，还原主链**\ （直接决定 grunt 还原对真实产物的可用性，都是语料高频面）：

1. string-encrypt 还原面（已处理完）：\ ``grunt.StringPoolFold`` 承载（v2/v3 同一 residue），池链三件套结构自证 + 自逆 XOR 流静态复算。boar 语料验证：Grunteon 全链产物把 clinit 的 arrayInit 调用藏进 indy 蹦床，折叠面在 indy 还原（缺口 #2）打通后才能接通。
2. indy 直接解密还原（缺口 #2 已处理完）：\ ``grunt.IndyRestore`` 承载，simple 形三段串按 ``encrypt`` 自逆 XOR 纯算术直解 + 三路键链（同名解密器/BSM 体跟随/名字族推导，覆盖 NativePipeline BSM 壳死现场）。heavy 形 owner 经 BSM 体 generalKey 解一层 + owner 类 ``@Metadata``\ （mv≥100 检查）d1/d2 数组 ``data1 % 59557 == magic2`` 自证回填（ ``Era::Grunt3`` 检查）。boar 语料（Grunteon 全链）78154 指纹站点还原 30924，组合链接通 StringPoolFold 0 → 19244 折叠；材料通道（RuntimeMaterial 联动）曾静态不可解 fail-closed 挂 ``materialLane``\ （42228）， **缺口 #15 联动面已处理完** ：\ ``grunt.MaterialKeyRestore`` 清洁口径静态折回 key 后第二轮本 pass 消化（boar 专链 42228 → 5060，余 5060 = native materialKey 类）。 ``IndyFold``\ （模拟执行）作为备用还原手段。
3. InvokeDispatcher 调用点反查（缺口 #3 已处理完）：\ ``grunt.DispatcherRestore`` 承载，头锚（ILOAD key 槽 + TABLESWITCH default 回跳方法头 + 无异常表）钉分发器、逐 case 直线转发体重建键 → 目标表、调用点 key 常量反查回填真实 invoke（原地换 + 末段补位/key 擦除，null 跑 ∈ [addNulls, N] 核对）；分发器本体与键表不删，交 DeadCode/DeadMember 回收。正例 jar（Grunteon 仅 InvokeDispatcher 自产）1181 站点回填 1081，residual=49 为宿主方法 Strict 帧重算既有冲突（无编辑探针复现）fail-closed；boar 全链 jar 不覆盖（config 顺序 InvokeDispatcher 先于 CFF/ReferenceObfuscate，5827 desc 形候选全数头锚缺席），组合链 ``IndyRestore,DispatcherRestore`` corpus-regress 绿 + JVM verify 零新增。
4. CFF 反摊平（缺口 #4 主面已处理完；processor 键解锁链已实现至静态天花板）：\ ``grunt.CffUnflatten`` 承载，岛识别（state 槽 confinement + ``[ILOAD s; TABLESWITCH]`` 键窗 + alias face ∈ case 目标）→ 桥 span 常量闭链静态求值回查键表 → 前驱直连真实块（出表键剪到 default，语义安全下限恒等）+ 岛/纯形桥整擦 + scramble 剪枝 + 入口块归位（异常保护区/junk 前缀退蹦床）+ handler 桥重绑 + 位置边守卫（条件载体紧邻岛头补 GOTO 钉落空臂）。boar 全链 jar 9880 岛还原 1481（类 653，failed=residual=41 fail-closed 如实计），只有 InvokeDispatcher 的正例 jar 290 候选全数反误伤产物逐条目 CRC 全等，corpus-regress 绿 + JVM ``-Xverify:all`` 基线同挂零新增 + 类数 3065 不增 + 双跑一致。 **processor 键解锁链实现 **\ （IndyRestore 还原 ``__grt$indy$`` 转发体内 indy → ConstantCall 同类豁免 + Math.max 接管折转发 → 本 pass processor 桥可解）：链上 ConstantCall 7→687、key-processor 桶 6201→6121（66 岛转 solvable 进改写 / 14 转 foreign-ref）、islands 1481→1547，DispatcherRestore 仍 0。 ** 余量** ：processor 桶剩余 6121 为静态天花板，boar 配置 ``keyProcessorNativeCandidateRatio=1.0`` ，\ ``(II)I`` proc 转发 6117 个恢复目标 100% native 无体（算术随 native pipeline 出 jar），无 native 库无解；SSA exp 变体（LookupSwitch + per-case stub + passArgs 编组）不同构遗留。

**P2，加密与控制流扩展面** ：

5. **ConstPoolEncrypt 调用点回填** （已处理完）：\ ``grunt.ConstPoolRestore`` 承载（伴生类形状钉 + 数值面 ``ConstantValue`` 属性与容器级零写 + 字符串面解密器双通道解码：24 条模板钉形走逐码元自逆 XOR 直解、钉形失败不拒类走求值通道（libjsim 真跑解密器，junk 包裹 fork 变体），``GETSTATIC`` 调用点原地换回常量装载），链上 NumberFold/ConstValuePromote 先行折供给链；伴生类本体不删交垃圾类面（#14），HeavyEncrypt 二次加密面逐类 fail-closed。语料验证登记见 :ref:`corpus-ledger`\ 。
6. **ConstBuilder 折回** ：switch 全表静态求值（操作数全为 ``ConstantValue`` 字段 + 封闭 IXOR），真 case 产出折回单条 ``ldc`` 。
7. **ArithmeticEncrypt/ArithmeticSubstitute 恒等简化** （已处理完）：\ ``grunt.ArithFold`` 承载（v2 ArithmeticEncrypt + v2 MutateJumps 直线形 + v3 ArithmeticSubstitute 三面，两代生成源逐字相同），替换八式形状钉 + 精确代数恒等（值逐位相等）折回单条指令，常量窗整窗归一（净栈效恰一个值 ∧ 全部非 NOP 指令参与，JVM 精确语义求值、除零不折）折回单条常量装载；缺口 #18 登记的 MutateJumps 直线形即常量对实例，归并本面不另建还原面。语料验证登记见 :ref:`corpus-ledger`\ 。
8. **NumberEncrypt v2 IAND/LAND 位拼模板扩面** ：\ ``first & second = v`` 对（int/long）加入 NumberFold 形状清单，已实现（六式，样本 B 位拼面 354090 全折、旧四式站点数逐位不动）；v3 无此形态，仅 v2 语料需要。
9. **MethodExtractor 还原** ：透传桩与伴生方法体合并、owner 类 accessor-getter/setter 内联、伴生类删除（面大价值中）。
10. **ControlflowJump processor 检查与异常桥 ** ：异常桥还原（面 b 已处理完）：\ ``grunt.ExcBridgeRestore`` 承载，形状钉（trap 四条逐字 + 保护区恰罩四条 + handler 恰 ``POP; GOTO`` + catchType 恰 RuntimeException）钉合成蹦床、异常表核对检查反误伤，\ ``replaceAllUses`` 原边直连真实目标 + 位置落入 GOTO 钉 + 表项删除/锚吸附 + 六受害者单批擦除 + Strict 帧链重算（判据与语料验证见 ``doc/exc-bridge-restore.rst`` ；boar 对照 jar no-op、样本 A 13566 站还原 13442 residual=0、独立 census 复扫 bridge=0、\ ``-Xverify:all`` 失败集与输入全等、双跑 CRC 全等；链序在 ``CffUnflatten`` 之前，岛还原 1281→1543）。 ** 重启条件兑现** ：样本 A（ ``exceptionBridgeChance=1.0`` ）按源锚形态重启，14 测新增（jasm 夹具：跳转边/位置落入/真实区共存+锚吸附/相邻双 trap/覆盖检查/X 互撞检查拒绝面 + JVM ``-Xverify:all`` 实跑）。processor 检查还原（面 a 已处理完）：\ ``grunt.GateProcessorFold`` 承载，(III)I ActionDesc 形状钉 + 三常量实参 + compareValue 常量走廊（ ``[INT; (INT; IMUL|IAND|IOR|IXOR)*]`` 栈机折叠）+ IF_ICMP 前窗逐字钉形，跨方法求值器（jsim 编排层递归穿透 ``__grt$indy$`` 转发器）+ ``Integer.rotateLeft(II)I`` 接管（PredicateStep 盐旋转步生成面）+ 生成面 ``require`` 恒真自证检查，折叠落 PredicateFold TAKEN 形 ``[ICONST_0, IFEQ→真臂]``\ （判据、算法、前后 jasm 对照与语料验证见 :ref:`gate-processor-fold-spec`）。 **前批3387 indy_body + 2872 impure + 0 pure之谜底 **\ （普查口径伪象）：ConstantCall 的跨方法求值器早把纯算术 proc 调用折成常量（样本 A 实测 5814 个 gate 调用），幸存 gate 是选择偏差人群，真余量 = rotateLeft 体（jsim 缺 JDK 数学面求值 EvalBadState）4282 + TLR 面 889 + material 类转发体（indy 未还原）3576。 ** 还原实测 ** ：样本 A 单跑 7643/7643 折叠（classes=721 failed=0 residual=0）；组合链 ``…ConstantCall,ExcBridgeRestore,GateProcessorFold,CffUnflatten`` 上 4282/4282 折叠（四上游 pass 锚点逐项不动、CffUnflatten islands 1543 不变）、boar 对照 jar no-op（类条目 CRC 逐条全等）、\ ``-Xverify:all`` 失败集与输入全等、双跑 CRC 全等、13 测新增（jasm 夹具清单见 :ref:`gate-processor-fold-spec`）。 ** 余量** ：material 类 gate 转发求值不可解（缺口 #15 联动面，material_key_restore / indy_restore 本体禁碰，链序实验 0 火）；TLR 运行期随机面永久排除（窗口变体 ``tlrWindow`` 桶 + 体引用 ``EvalBadState`` 桶）；上游 ConstantCall 专用求值面 2453（str/num/reflect/indy/Math.max 扩展包语义，本 pass 刻意不装包 fail-closed，调用已被上游折叠）；其 const-vs-const 比较面是 PredicateFold 判据（主线 pass，v3 链未编入）。boar 原停报双证（processor 体 100% 随 NativePipeline 出 jar → 单跑 sites=0 no-op）与boar 站点普查须按 StackMapTable 帧根做校验器式可达遍历注记仍有效。

**P3，清理、检测与基线面** ：

11. **StringEquals 语义还原** （已处理完）：\ ``grunt.StringEqualsRestore`` 承载（v2 ``StringEqualsRedirect`` / v3 ``StringEqualsOptimize``\ ，hashCode 比较/``==`` 换回 ``equals`` 含 ``equalsIgnoreCase`` 面，两代生成源输出逐字相同一套形状表），判据 = 窗口逐条 op + 成员引用形状钉（equals 面 6 条 / equalsIgnoreCase 面 8 条）锚点原地换 ``String.equals`` / ``String.equalsIgnoreCase``；hashCode 比较对碰撞串误判相等是生成侧自认损失面，还原取严格 equals 语义，如实登记。语料验证登记见 :ref:`corpus-ledger`\ 。
12. **ParameterObfuscate 收窄** ：私有方法 desc 参数收窄 + 头部 checkcast 删除。
13. **噪声与检测面清理** ：Watermark（两代水印字段/方法）、Crasher/DecompilerCrasher（massive Signature 属性）、AntiLLM 四渠道噪声、HWID 校验调用摘除（删除需 CFG 副作用证明）。 ** 属性体面已收半边** （libjbc 0.4.29）：SourceDebugExtension（JVMS 4.7.11 纯字节串属性）在 cp_compact 闭包放行、体原样透传——junk 体不再整类冻结压缩（260705 语料 79 类解锁，台账解锁批 ①）；massive Signature 属性**本体摘除面**仍挂本缺口，且已实证它是 CFR 0.152 的硬死点（t3_full 152/152 类全带，逐类扫描 input 0/152 = 还原产物 0/152 同因同败 ``ConfusedCFRException: Prototype``——含「形合法实非法」方法签名 ``Ljava/lang/Class<...>;`` 缺括号变体；JVM 无视该属性故运行时无恙；crasher 面语料的 CFR 门 = 输入/产物同败对账）。 ** 新登记计数反射脸** ：JavaObfuscatorTest 2.1 按 ``getDeclaredMethods().length`` 计数——名字面保护（注解/池名/indy 目标）对按数反射天然失明，成员删除的判据必须带形状证据（Grunteon 全员撒 ACC_SYNTHETIC/ACC_BRIDGE 位、实例方法 acc=0x1041 实证，DeadMember gateB 位证据因此限 static 面，台账解锁批 ⑤）。
14. **垃圾类删除面** ：v2 ClonedClass/TrashClass + v3 TrashClassGenerator，零引用 + 生成面形状识别，删除需引用普查确认（反射/资源文本引用）。
15. **probeEra Grunt2/Grunt3 可靠判据** ：Grunt3 半边已处理完（随 indy_restore 落 era.hpp：Grunt BSM desc 指纹 ∧ 类级 ``@Metadata`` 注解或 ``grunteon`` 命名空间，双半边钉版本）；Grunt2 半边仍缺，两代 ``U+200E`` 尾缀同源、indy BSM desc 逐字同源、模板无稳定差异，待语料积累出真正版本敏感的残留形状再补（era.hpp 明文记载）；v2 现场的 heavy 面暂不处理（simple 面全结构自证，不依赖版本判断，不受影响）。

**追加缺口（P2 级）** ：详见 v2 分篇 Controlflow 与 NumberEncrypt 条目。

16. **v2 Controlflow junk 调用残留与陷进环** ：假跳转步（ ``BogusConditionJump`` ）落空侧的垃圾调用点（垃圾值 + 压实参调用 + 按返回类型死出口）不是常量可裁决形态，PredicateFold 判据天然不吃； ``TrappedSwitchCase`` 陷进环与 ``ChaosSwitch`` 双 switch 环的假 case 互跳结构无专项还原面，还原思路是静态识别出假 case 互跳闭包与校验器式不可达，剪枝后交给 PredicateFold/DeadCode。语料：v2 测试 jar 的 switch 总量 121236（其中 104263 是假跳转前窗的 GETSTATIC+IXOR），都属 Controlflow 的 switch 保护与 ChaosSwitch 形态，见 corpus-ledger §2。
17. **v2 NumberEncrypt arrayed 池折回** ： ``long[]`` 池字段 + ``<clinit>`` 一次性解密方法整体填充（外层 key 走 classic 跨度加密）+ 调用点 ``GETSTATIC pool; idx; LALOAD`` （float/double 外包位形换算），池读形态与 #1 字符串池同构，还原要先把外层 classic 走廊折掉。开关默认关闭（ ``Arrayed=false`` ），生成测试 jar 时也没开，现有语料里没有这个形态。
18. **v2 MutateJumps 直线常量对折叠（登记条目，还原面归 #7）** ：第 2 步跳转变异（ ``ControlflowTransformer.kt:123-136`` + ``MutateJumps.kt`` ，默认开、 ``MutateRate=10`` ）的 ``obfuscateBasic`` 四形中，IAND 位拼形与 #8 同构已随 NumberFold 折掉；IADD/ISUB/IXOR 直线形（ ``INT(v±k); INT(k); IADD/ISUB`` / ``INT(v^k); INT(k); IXOR`` ）不是 NumberFold 认的形状，形状上属 #7 恒等简化面（两条紧邻常量 + 一条算术指令），由 #7 的简化链一并处理，此处只登记行为来源（v2 Controlflow 的第 2 步跳转变异）与站点形状，不另建还原面。

.. _obf-behaviors-corpus:

6 语料登记（重启批）
----------------------

#10 重启条件已满足，样本 A ``boar-main-v3-full-nonative``\ （v3 全链关 native，3074 类，(III)I 直线纯算术体 11453、异常桥站点 13566、native 方法 0，#5-#8 之外的两前提双证转正）；样本 B ``boar-main-v2-full``\ （v2 全链 36c0eef，10486 类）解锁 P2 #5-#8 语料，伴生类面 2516 类/938409 GETSTATIC、AE 恒等面 const-fed IXOR 410536（输出形被 ConstPoolEncrypt 打散）、IAND/LAND 位拼 212222/141868（+GETSTATIC 喂给 25372/32718）、ConstBuilder 0（301 序饿死）。
两份 jar 路径 + sha256 + 全量站点量表 + 生成命令见 :ref:`corpus-ledger`。

.. _obf-behaviors-driver:

7 通用折叠面清单与驱动器契约
------------------------------

.. _obf-behaviors-generic:

7.1 通用折叠面（非单一 transformer 对应，两代共用）
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

上两表按 Grunt 行为逐条归属；另有一组 pass 的面是**通用字节码事实**（不挂
家族行为标签，任何产出该形状的混淆器/编译器都适用），驱动器主线成员在此
登记归属：

- ``grunt.TrampolineSqueeze`` / ``grunt.DeadCode``（穿线面）：goto/goto_w
  目标 = 紧邻落点的冗余无条件跳转，判据走 ``rewrite_tools::isFallthroughGoto``
  单源（backing 布局真值）。混淆器用（goto + 落点标签）对劈开实参区/
  常量表达式；标签残渣处置（无引用者销毁）是擦写不变量。
- ``grunt.ConstantCall``：常量实参纯静态调用折叠。容器外目标唯一入表面 =
  JDK 全序纯内建白名单（ ``kExternalPureBuiltins`` ，成员登记三件套规则见
  ``src/grunt/constant_call.cpp`` 表注；现成员 ``Long.parseUnsignedLong``，
  池读下标键面解锁器）。
- ``grunt.ConstMethod`` / ``grunt.DeadMember`` / ``grunt.DeadCode``（死区
  面）/ ``grunt.JunkRegionStrip`` / ``grunt.FlagsStrip``：清理与折叠的
  通用面，行为归属按两表各行的 residue 列。
- ``grunt.PoolArrayFold`` / ``grunt.IndyFold``：求值通道（池 clinit 跨方
  法求值压池 / BSM 模拟执行），形态由目标结构自证。

Kasumi 专面（ ``IndyDecode`` / ``StringCacheFold`` / ``MetadataStrip`` ）
不在本篇，见 :ref:`obf-behaviors` §2 文档索引指向的 ``doc/kasumi-verify.rst``。

.. _obf-behaviors-contract:

7.2 驱动器契约（不动点轮驱动， ``grunt_cli --converge``）
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**判据**：容器字节面指纹（ ``src/grunt/converge.hpp`` ）——class/unknown
资源按内容 FNV1a-64 入账、manifest 只记存在性、条目按 uri 排序消遍历序。
整条 ``-p`` 程序重复执行到轮前轮后指纹相等。字节真值口径：任何 pass 漏报
的写回都逃不过比对，不依赖 PreservedAnalyses 聚合（runContainerPipeline
不外露聚合 PA）。

**结构**： ``-p`` 是**每轮**程序（同轮排序保效率，下表约束在默认主线
``grunt.defaultMainLine()`` 已排好）；驱动器加**轮维度**，跨轮约束机械吸
收。每轮管线 + 层次图整体重建（工厂按 build 重建 pass 实例，类字节跨轮
变化不得复用陈旧骨架）。轮上限默认 12（实测样本 A 收敛 ~9 轮），达上限
未收敛如实报告仍导出。 ``--converge`` 强制 ``GRUNT_SLICE_SEC=0``——片钟
截断会让整轮空转（墙钟尽后 admit 全拒）→ 指纹不变 → 假收敛。

**链序约束 → 轮维度吸收对照**：

.. list-table::
   :header-rows: 1
   :widths: 34 22 44

   * - 约束
     - 来源
     - 吸收方式
   * - Squeeze / NumberFold 先于实参收集面
     - README 硬约束
     - 同轮排序（默认主线已排）
   * - ConstantCall 先于折叠 / 清死
     - README 硬约束
     - 同轮排序
   * - PredicateFold 之后必须跟 DeadCode
     - :ref:`dead-code` 链序约束
     - 同轮排序；死块擦除新退化的冗余 goto 由下一轮穿线（样本 C
       b.main 实证：第 1 轮擦走廊，第 2 轮穿线）
   * - DC 清完走廊后下游再跟折叠轮
     - :ref:`dead-code` 链序约束
     - 下一轮 ConstantCall/NumberFold/ArithFold（需要池读面时
       PoolArrayFold）重扫，标签锁拿掉后的常量窗口才收拢（样本 C 实证）
   * - ExcBridgeRestore 先于 CffUnflatten
     - exc-bridge-restore.rst
     - 同轮排序
   * - MaterialKeyRestore 之后 IndyRestore 第二轮
     - material-key-restore.rst
     - 下一轮 IndyRestore 重扫消化 materialLane 桶
   * - ArithFold 链上重复收敛
     - arith-fold.rst
     - 下一轮重扫至无窗口
   * - ProxyInline → ConstantCall 两跳
     - proxy-inline.rst
     - 下一轮 ConstantCall 重扫
   * - ConstValuePromote/ConstPoolRestore 供给 ArithFold
     - const-pool-restore.rst
     - 同轮排序 + 下一轮重扫兜底
   * - **NumberFold 必须排在 CffUnflatten 之后**
     - 验收阵 t3_full 实证（台账验收阵批）
     - 同轮排序；轮维度吸收不了顺序错误（每轮同错）：NumberFold 先行
       让 CFF span 形被判 ``state-polluted`` 370/404、岛还原 58→0；默认
       主线已移到 CffUnflatten 之后
   * - DeadMember 排在 junk 清理预热之后
     - README
     - 前几轮供给链先折，DeadMember 轮次自然递减（台账实证 414→2→0）

**收敛实证** —— :ref:`corpus-ledger` 驱动器批：样本 C（v7r10_q8）默认主线
4 轮收敛（158→70→1→0），调优 26-pass 程序 3 轮收敛（158→17→0），双跑逐条
目 CRC 全等，行为 9 PASS 与基线同分布，CFR 210 类零错，残差 = v29 水平。
README 原「单轮跑到收敛要重复整条链（判据 PredicateFold 归零）」的手工口
径升级为本机制：指纹判据覆盖全 pass，不只 PredicateFold。
