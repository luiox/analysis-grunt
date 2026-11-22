.. _junk-member-strip:

junk_member_strip：junk 成员层链断（clinit 区域手术 + junk 成员删除）
================================================================================

Grunteon 把 junk 键初始化段织进 ``<clinit>`` ：ldc2_w 常量 + nop 对 +
ladd/lsub/ior/lxor 混算 + ``invokestatic`` 调 ``$gg*`` junk 探针方法 +
条件跳转/陷阱尾，这段引用 junk 字段（\ ``$g*``\ 形与数字开头形）与 junk
方法，方法又引用伴生类（\ ``$PredicateProcessor$``\ /\ ``$KeyProcessor$``
形）——整条链因此对引用普查可见，DeadMember 无法回收。本 pass 断链：
clinit junk 区域长度保持 NOP 化 → CP 压缩 → 活引用普查 → junk 成员删除
→ 再压缩，伴生空壳交下游 DeadMember。


判据
----

**先实证后判据**\（jsoup dec_jdk 655 类全量普查钉形）：junk 字段 1081
（\ ``$g``\ 形 1040 + 数字开头 41）、\ ``$gg``\ 探针方法 260——普查数与
人工逐类核对全等。

区域判定（scan 一次定死，rewrite 只写 0x00）：

1. **栈槽模拟器**：完整 JVMS 槽位效应表（J/D 双槽、描述符解析）——
   ``iinc``\ (0x84) 曾因落错桶被当 (1,0)，295 个方法假 underflow 的根因，
   表错即 fail-closed 放弃；
2. junk-CFG 闭包 + 值馈送链式回拉（G4：净槽位效应 ≥ 0 的整链收——
   per-insn 回拉会把消费闭包外锚值的 pop 卷进区域，实证修掉）；
3. 前沿收缩 ≤64 轮 + 孤儿容许（诱饵形门）+ 夹层真实块 ≤1。

四道结构门（真实调用者/读者拒证）：

- G1 junk 命名方法只作删除候选；G2 区域不得从方法入口起；
- G3 区域须有 ≥1 junk 字段种子；G4 馈送链净效应闸。

帧面 F1-F4（HotSpot 逐错钉出）：

- 保留尾 return 及其陷阱尾（\ ``pop; goto``\ 回跳保留尾的蹦床）不 NOP
  ——其帧声明状态对原指令流成立；
- 段内 full 帧在「栈深一致 + locals 内容 == 初始内容 + 其前无真实
  locals 写」时保留（locals 类型态基线由它承载，否则构造器
  ``initializedThis`` 塌回）；
- 保留帧核对栈深；locals 内容沿原链/保留链双轨对照；真实分支不得指向
  帧被摘 pc；同帧 delta 重排 + 超界升格 extended。


改写
----

NOP（长度保持，偏移/SMT/LVT/LNT 不动）→ 候选类 CP 压缩（非候选类不搅动，
干净类字节零变化）→ 活引用普查（census-blind 整轮放弃）→ 倒序 erase →
再压缩。


验证
----

jsoup dec_jdk 全量实测：129 个 NOP 区域 / 9207 字节 / 探针方法 8/260、
junk 字段 32/1081 删除（rejected 1301 全有 abandon 计数）；改写 jar
全量 655 类 JVM 链接 + clinit 初始化 ok=655 fail=0；\ ``org.demo.App``
14/14 语料对拍逐字节一致；伴生空壳进 DeadMember 删除面接力实证。

放弃面直方图（逐项实测，宁留方向）：rewrite-sim underflow 211（栈耦合：
探针值被真实体消费）、indy 效应未知 65、G2 入口起区域 47、multi-blocks 33、
real-branch-into-dropped-frame 25、kept-return 槽深 15 等。**与无帧面
原型的差额**：原型报 278 区域/53 探针/212 字段，但其产物 220 类
VerifyError（tier-2 吞真实块 + 摘帧破坏 locals 基线）——本实现删除集是
原型的保守子集，JVM 可验证性优先于删除量。

jasm 夹具 9 测试（五面 ①-⑤ + 陷阱尾/SMT 帧/DeadMember 接力/真实 jar
回归——jar 缺位自动 skip）。
