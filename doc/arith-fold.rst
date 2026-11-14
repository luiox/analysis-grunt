.. _arith-fold:

arith_fold：算术替换 junk 还原与常量对折叠
==============================================



形态
----

两族输入，一个 pass 承载，来源三面：v2 ``ArithmeticEncrypt`` 、v2 Controlflow 跳转变异 ``MutateJumps`` 的直线形、v3 ``ArithmeticSubstitute`` （ ``Hidden`` ）；替换八式两代生成源输出逐字相同。

一是替换形状：生成侧 replaceINEG/replaceIAND/replaceIOR/replaceIXOR 的逐字输出形，共八式。操作数是栈上既有值（通配），窗口本身是纯固定指令序列：

::

     INEG 形 0：  ICONST_M1 IXOR ICONST_1 IADD                → INEG
     INEG 形 1：  ICONST_1 ISUB ICONST_M1 IXOR               → INEG
     IAND 形：    SWAP DUP_X1 ICONST_M1 IXOR IOR SWAP
                  ICONST_M1 IXOR ISUB                        → IAND
     IOR 形：     DUP_X1 ICONST_M1 IXOR IAND IADD            → IOR
     IXOR 形 0：  DUP2 IOR DUP_X2 POP IAND ISUB              → IXOR
     IXOR 形 1：  DUP2 ICONST_M1 IXOR IAND DUP_X2 POP
                  SWAP ICONST_M1 IXOR IAND IOR               → IXOR
     IXOR 形 2：  DUP2 IOR DUP_X2 POP ICONST_M1 IXOR
                  SWAP ICONST_M1 IXOR IOR IAND               → IXOR
     IXOR 形 3：  DUP2 IOR DUP_X2 POP IAND ICONST_M1 IXOR
                  IAND                                       → IXOR

八式都是精确代数恒等式（生成侧自证：替换前后值逐位相等），窗口折回单条指令是纯代数改写，不依赖生成侧意图，用户代码里恰好出现同序列时折叠同样保语义。

二是常量对折叠：v2 ConstPoolEncrypt 把常量改写成伴生池字段读取后，替换形状与 junk builder 的常量输入整片变成 GETSTATIC 喂给（ ``grunt.ConstPoolRestore`` 回填 ``ldc`` 后这些窗口重新封闭成纯常量算术）。窗口从常量装载锚起，纯常量装载 + 封闭数值运算逐条求值，整窗归一（净栈效恰一个值且窗口内全部非 NOP 指令都参与该值）才折成单条常量装载。求值按 JVM 精确语义：int/long 全指令回绕、移位按 5/6 位掩码、除零不折（保留原地异常）、F2I/D2I 族 NaN 归零与饱和钳位、浮点按 IEEE 运算逐位复算。

MutateJumps 直线形（ ``INT(v±k); INT(k); IADD/ISUB`` / ``INT(v^k); INT(k); IXOR`` ）与 TRIP1 形（ ``A ICONST_M1 IXOR B IAND`` 常量喂给）都是常量对折叠族的实例，不单列形状（行为来源登记在缺口 #18，还原面归缺口 #7 即本面）。


判据
----

- 替换形状逐条 op 吻合（含固定常量 ICONST_M1/ICONST_1），窗口内除锚点外无标签（合并点 = 外部到达路径栈形耦合，放弃）。
- 常量窗口的每条指令可求值（纯常量装载 + 封闭运算白名单），窗口上限 16 条非 NOP 指令，白名单外指令即停窗。
- 常量窗口整窗归一：净栈效恰一个逻辑值 + 全部非 NOP 指令参与，二者缺一不收（窗口里留死值的形态不折）。

替换八式是精确恒等式、值逐位相等（S1 语义后果）；形状逐条钉形是结构面（S2），两代生成源逐字相同，换代换形状即停折。


改写
----

按 ``srcPc+op`` 重定位锚点后全量复核（同方法前站改写后不盲改）。替换形状锚点原地改单条目标指令（保 ``srcPc`` ），常量窗口锚点原地改结果常量装载；窗口其余非 NOP 指令擦除（eraseBatch kRejectIfUsed 拒收即整站还原放弃）。帧链走 mutateMethodWithFrames 正统写回，前置 roundTripSafe。替换形状还原后暴露的新常量窗口（如 ``LDC a; LDC b;`` + 还原出的 IAND）本轮不展开，链上重复 ``grunt.ArithFold`` 收敛（与 IndyRestore 第二轮同一口径）。

链序：排在 ``grunt.NumberFold`` 与 ``grunt.ConstPoolRestore`` 之后。NumberFold 的六式跨度先吃自己的面（旧式站点数不受本 pass 影响，加性恒等），ConstPoolRestore 回填的常量是本 pass 常量窗口的主要燃料。


已知边界
--------

- 栈操作（DUP/SWAP/POP 族）不进常量窗口白名单：常量喂给的替换形状由形状族先行还原（还原后剩纯常量对，下一轮折掉），不在窗口里模拟栈洗牌。
- FCMP/DCMP/LCMP 不进白名单（确定性可折但语料无此面，不扩）。
- 除零（IDIV/IREM/LDIV/LREM 除数常量 0）不折：原地保留 ArithmeticException 语义。
- 用户代码恰好含替换形状序列时同样折叠：八式是精确恒等式，值逐位相等，无改错风险（如实登记）。

判据源锚与实现细节见 ``src/grunt/arith_fold.hpp`` / ``arith_fold.cpp`` 文件头注释。
