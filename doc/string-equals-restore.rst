.. _string-equals-restore:

string_equals_restore：hashCode 比较换回 equals
===================================================



形态
----

Grunt 两代把字符串相等比较改写成 hashCode 比较：v2 ``StringEqualsRedirect`` 与 v3 ``StringEqualsOptimize`` 的生成源输出替换序列逐字相同，一套形状表。equals 面（6 条）：

::

     INVOKEVIRTUAL java/lang/Object.hashCode ()I
     INVOKESTATIC  java/lang/Integer.valueOf (I)Ljava/lang/Integer;
     SWAP
     INVOKEVIRTUAL java/lang/String.hashCode ()I
     INVOKESTATIC  java/lang/Integer.valueOf (I)Ljava/lang/Integer;
     INVOKEVIRTUAL java/lang/Integer.equals (Ljava/lang/Object;)Z
                                                 → INVOKEVIRTUAL String.equals

equalsIgnoreCase 面（8 条）：两端各前置一次 ``INVOKEVIRTUAL java/lang/String.toUpperCase ()Ljava/lang/String;`` ，第二端 hashCode 走 String.hashCode → INVOKEVIRTUAL String.equalsIgnoreCase。

操作数（receiver 与比较串）在窗口下方，通配。窗口内 5/7 个成员引用逐字钉死（Integer.valueOf/equals、Object.hashCode、String.hashCode、toUpperCase），S2 形状钉。


语义口径
--------

hashCode 比较对碰撞串会误判相等（生成侧自认的损失面），换回 equals 是生成面的逆映射（还原到原程序语义）。接收端/实参的验证器类型由窗口内的 String.hashCode / String.toUpperCase 调用自证（原代码在同一栈位上调过 String 成员，恢复 INVOKEVIRTUAL String 系成员验证器照过）。


判据
----

- 窗口逐条 op + 成员引用精确吻合（NOP 夹层跳过）。
- 窗口内除锚点外无标签（合并点栈形耦合，放弃）。


改写
----

按 ``srcPc`` 重定位锚点后全量复核（同方法前站改写后不盲改），锚点原地换目标 invoke（setMember，保 ``srcPc`` ），窗口其余非 NOP 指令擦除（eraseBatch）。帧链走 mutateMethodWithFrames 正统写回，前置 roundTripSafe。


已知边界
--------

- 用户代码恰好含同序列时同样还原：序列语义与 equals 等价面只在哈希碰撞串上分叉，还原方向是取严格语义，如实登记。
- 生成侧未来若换比较载体（Long.compare 之类），形状表不认，fail-closed。

判据源锚与实现细节见 ``src/grunt/string_equals_restore.hpp`` / ``string_equals_restore.cpp`` 文件头注释。
