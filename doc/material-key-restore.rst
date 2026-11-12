.. _material-key-restore:

material_key_restore：RuntimeMaterial 材料通道 key 静态折回
====================================================================================================



形态
----


Grunt v3 ``RuntimeMaterial`` （AntiDebug，grunt3@e41e852 ``RuntimeMaterial.kt`` 逐行）给带材料草稿的类注入四个静态字段（shareA/shareB/canonicalKey 即keyField，long + poison int）， ``materialKey()`` （ ``createMaterialKeyMethod`` 生成面，boar-main.jar 逐字节验证 9 条逐字）：

::

   private static synthetic bridge int materialKey() {
       return (int) ((keyField ^ (keyField >>> 32)) ^ poison);   // Long.foldToInt ^ poison
   }


``ReferenceObfuscate`` （同 commit ``ReferenceObfuscate.kt`` ）在带材料草稿的类里生成 **材料通道解密器** （ ``(String;)String`` ，窗口与普通 lane 同构，多一层 ``materialKey()`` 调用），有效 key = ``K ^ materialKey()`` ：

::

   // 材料解密器窗口（boar zA 类验证）：
   charAt(s, i); LDC K; INVOKESTATIC <self> materialKey()I; IXOR; IXOR; I2C


上游供给链 ``grunt.IndyRestore`` + ``grunt.StringPoolFold`` 跑完后，材料初值链 **全静态** （number-reobf 层把 ``clinitMaterialInit`` 的 ``LDC2_W`` 常量包进基 32 字符串通道的产物形）：

::

   // <clinit>: keyField 初值（Form B）
   ldc "<基32串>"; bipush 32;
   INVOKESTATIC <parse 蹦床或 Long.parseUnsignedLong 直连> (Ljava/lang/String;I)J;
   ldc2_w C; LXOR; PUTSTATIC keyField J
   // poison 初值：纯常量走廊（boar 验证 `ldc X; i2l; ldc Y; ixor` ≡ 0）


boar 语料验证：每类恰好一个材料解密器， ``materialKey()`` 恰被它调用一次（xref 全查）；656 个材料类里 70 类的 ``materialKey()`` 被 NativePipeline摘成 ``native synthetic`` 空壳，key 不在 jar 里，本 pass fail-closed 如实挂 residual。


为什么可以静态还原
------------------


要折回材料通道，得回答 ``materialKey()`` 在清洁运行环境里返回什么。 ``ReferenceObfuscate`` 构建期算死 ``simpleDecryptKey = decryptKey xor seedFold`` （生成面源码），即 **有效 key 在构建期就是常量** ；运行期岔路只剩RuntimeMaterial 自己的反调试扰动。生成面源钉（grunt3@e41e852``RuntimeMaterial.kt`` perturbation）：

- clean 扰动 **只碰 shareA/shareB** （ ``shareA += delta; shareB -= delta`` ，和不变），canonicalKey 与 poison 不动；
- 可疑扰动（检测到调试器才走）才写 keyField/poison，且写点带守卫指纹（ ``INVOKESTATIC guard()I; DUP; IFEQ clean`` ）。

所以 **清洁运行环境语义下 ``materialKey() ≡ fold(keyField 初值) ^ poison初值 ``** ，两边都是 clinit 里的常量闭链，C++ 直解（ ``parseUnsignedLong`` 、 ``fold`` 、XOR 走廊都是固定 JVM 语义，不需要模拟执行；这与 NumberFold 折基32 跨度的口径同源）。这就是还原口径： **deobf 的既定口径是清洁运行环境，反调试扰动路径（guard 命中时故意用错 key 自毁解密）不在还原范围** ，这不是对运行期岔路的静态裁定，是和混淆器为了保持功能必须如此同一级的语义边界。

判据全是容器自证事实（S2 结构 + S1 核对）， **无 Era 检查** （类级 Grunt BSM desc 指纹做家族锚，同 string_pool_fold 口径）：

- **S2** ：解密器窗口形（与 indy_restore 同一窗口回溯）+ ``materialKey()`` 体 9 条逐字 + 材料字段 private static + 写者普查（唯一初值写 + 其余写全为扰动窗口形 + 扰动写所在方法带守卫指纹）+ 初值跨度逐条钉形（Contiguous 无合并点）；
- **S1** ： ``materialKey = fold(keyField 初值) ^ poison 初值 `` ；核对 = 路由到本材料解密器的全部 BSM indy 站点按候选 key（= ``K ^ materialKey`` ）解出合法 owner/name/desc 且与 indy desc 核对全过，错 key 解出垃圾必死在这步，与 indy_restore simple 面同一自证逻辑。

任何一环不成立整类 fail-closed：初值表达式不可静态求值（含池读未折回的链序缺位）、字段出现非扰动窗口形的额外写、多个初值写、初值写不在clinit、守卫指纹缺席、 ``materialKey()`` 体非逐字形（native 化）→ 整类拒绝，字节逐位不动。


改写（与 indy_restore 的零侵入协作）
--------------------------------------------------


本残留只把材料解密器窗口里的 ``INVOKESTATIC <self> materialKey()I`` 原地换 ``LDC <materialKey>`` （srcPc 保留，帧链正统写回），窗口随即变成indy_restore 已有的 reobf 折叠键形（ ``LDC K; LDC MK; IXOR; IXOR; I2C`` ，key = K ^ MK）， **indy_restore 第二轮按既有 Plain 通道折回全部materialLane 站点** 。indy_restore 本体一个字节不动。

链序（外部知识）： ``grunt.IndyRestore,grunt.StringPoolFold,grunt.MaterialKeyRestore,grunt.IndyRestore`` ，材料初值跨度依赖StringPoolFold 折回的池明文 ldc；池读未折回的类 fail-closed（ ``initBlocked`` 独立计数，报链序缺位）。 ``materialKey()`` 本体、材料字段与守卫支路不删（折回后 materialKey 零引用，交下游 DeadMember/DeadCode回收）。


关键事实（判据的地基）
----------------------


- **CFF 摊平不挡窗** ：clinit 被 ControlflowFlattening 摊成 tableswitch状态机后，扰动窗口落进摊平块，窗口头（GETSTATIC）带块首标签，本pass 的窗口扫描允许头标签、拒绝窗口中段标签（合并点放弃），boar 全量656 类无一因 CFF 误拒。
- **守卫指纹只钉调用点** ： ``INVOKESTATIC <self> ()I; DUP; IFEQ`` 三条连续（guard 体读 ManagementFactory/环境变量找调试证据，内容不进判据）。无 DUP 的裸 ``GETSTATIC; ...; PUTSTATIC`` 同形窗口落 ``writerUnguarded`` 桶，不可证伪的扰动写按可疑处理，整类拒。
- **BSM–解密器配对是名字族不是同名** ：材料 lane 的解密器名 = BSM 名B[2, len-1)（boar zA：BSM ``6A1b27gwa9mAnYfh`` ↔ 解密器 ``1b27gwa9mAnYf`` ；plain lane 是同名 B[1,13)，两条 lane 配对面不同，取证定案）。BSM 体跟随路（体内 INVOKESTATIC 解密器）兜底覆盖改名差。
- **native ``materialKey()`` 是硬余量** ：NativePipeline 摘走方法体后 key只存在于 native 库，静态不可还原（70/656 类， ``keyBodyMismatch`` 桶，与 indy_restore 的 NativePipeline 检测-only 口径一致）。
- **poison 初值走廊可带算术** ：evalPoisonInit 用受限迷你栈机（常量装载/转换/整长算术白名单，多起点窗口求值，唯一值才放行），boar 验证形是 ``X^X ≡ 0`` ，白名单不猜堆访问与调用。
- **S1 核对零反驳** ：boar r2 产物上 586 类成立站点中 528 类带路由 indy站点，37168 站点全部按候选 key 解密 + desc 核对通过，keyRefuted=0；58 类窗口无路由站点，按结构 S2 证据成立（如实在账）。


与相邻形态的区别
----------------


- **indy_restore（materialLane 桶）** ：那边撞到材料窗口时静态不可解、fail-closed 计数；本 pass 把窗口折成它已有的 Plain 形，第二轮它按既有通道消化。两边零侵入，协作面只有 ``INVOKESTATIC`` → ``LDC`` 这一个改写动作，indy_restore 不需要知道本 pass 存在。
- **NumberFold** ：那边折藏形数值跨度为单条常量装载（基 32 parse 面同源）；本 pass 不折跨度本身，只在初值链上 ** 求值** （值进站点，跨度原样保留交下游）。基 32 语义共用（radix 32 严格解析），实现各自独立。
- **StringPoolFold** ：上游供给链，它把池读折回 ``ldc`` 明文，本 pass 的keyField 初值跨度才可静态求值；池读在场即 ``initBlocked`` （链序缺位独立计数，不与形状拒混桶）。
- **RuntimeMaterial 反调试本体 ** ：守卫支路、扰动写、材料字段全部 ** 不摘** （还原口径 = 清洁运行环境语义；扰动路径保留原样，guard 命中行为与混淆产物一致）。摘除面（检测/摘除缺口清单 P3）不在本 pass。
