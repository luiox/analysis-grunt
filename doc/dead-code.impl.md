# dead_code 实现笔记

## 数据流

`Site{uri, methodIndex, victimCount}` → `DirtySet{uris, methods, insns,attempted}`。SCAN 独立成段：不依赖其他残留的脏标记，管线里放最后，把全部上游改写产生的死块一次收走。

## 位置索引

可达性 BFS 需要 `InsnId → order 下标` 的映射，用 `mjbase::asm_cfg::InsnIdHash`（mjbase 公开头，本仓不自带哈希）。

## 异常表感知的可达性

`reachability()` 是唯一判定入口，SCAN 与 REWRITE 共用：

1. 入口 = `order[0]`，按 op 展开后继（`follow()` 把 LabelId 解析回 order 下标，`isEnd()` 无后继）；
2. 对每条异常表项，若 `[start, end)` 与可达集相交且 handler 未标记，则把 handler 标为根继续 sweep；
3. 重复 2 直到没有新标记（handler 可能落在另一条保护区内）。

`labelPos(label, allowEnd)` 的 `allowEnd` 区分两种语义：`end` 锚允许等于 `order.size()`（区间到方法末），`start`/`handler` 不允许。

## 异常表收敛（擦除之前）

`rewriteMethod()` 先扫一遍 `cu->exc`：

- 区间内无存活指令 → 丢表项，其 start/end/handler 三枚标签记入孤儿集；
- `start` 不在存活指令上 → `bindLabel(e.start, order[firstLive])` 搬移旧 Boundary 标签（句柄身份不变，表项引用不动）；
- `end` 不在存活指令上 → 吸附到其后第一条存活指令，找不到就 `bindLabel(e.end)`（方法末哨兵，无参形态）。

任一端点解析失败（`sBgn == kNo || sEnd == kNo || sEnd <= sBgn`）整方法放弃（return 0），不做半截改写。`excChanged` 但 `victims` 为空时也写回 `cu->exc`——异常表本身就是改动。孤儿标签销毁前清点：句柄无效（含 kNoLabel）跳过、孤儿集内重复句柄只销毁一次、被存活表项共享的保留——按 `cu->exc` 向量序线性清点，无集合遍历，双跑产物不引入遍历序依赖。受害者内部互指（死块内 goto）的 ControlFlow 标签不保护锚点，不阻擦除；若收敛逻辑漏处置某枚 Boundary 标签，`eraseBatch` 按锚定保护契约返回 Err，本 pass 保守放弃——fail-closed 兜底即断言。

## 修复记录（2026-11-12 修复批：端点吸附/删表路径整体不可达）

**根因**：收敛异常表时端点吸附用 `createLabel` 造新 Boundary 标签替换`cu->exc` 端点引用，但从不销毁/搬移旧 Boundary 标签——旧标签仍锚在被擦的死指令上。libjbc `CodeDom::eraseBatch` 契约「批内任一成员被存活Boundary 标签锚定 → Err 且整批不删」→ `rewriteMethod` 返回 0，整方法fail-closed 空转：scan 正确报站点，rewrite 字节逐位不动，文档宣称的两类改写（端点吸附 / 全死区间删表项）整体不可达。对照组实证：端点全落存活指令上的中段死块（`kMidRegionExcSource`）擦除正常，两组用例唯一差异就是死指令上是否残留旧边界标签。

**修法**：端点吸附改 `bindLabel` 搬移旧标签（`exc_bridge_restore` 的rebind 同款原语，异常表引用不动）；删表项的孤儿端点/handler 标签销毁（清点存活表项共享与孤儿集内重复）；吸附路径无任何 set/map 遍历新增，确定性不受影响。

**验证**：
- 单测：`kEndAnchorDeadSource`（end 锚死指令）/`kStartAnchorDeadSource`（start 锚死指令）/`kFullyDeadRegionSource`（全死区间）三存证用例由 fail-closed 断言翻转为文档行为断言（吸附后端点逐项断言、全死区间表项消失、死指令物理消失、handler 存活保持），对照组 `kMidRegionExcSource` 留作回归锚。注：夹具文本 `bipush N` 经 libjhasm `constantLoad` 规范化落 `ldc`（bipush/sipush/ldc 族文本并入同一语义常量不再保形），吸附靶断言按落码形态。`--gtest_filter='DeadCode*'` 9/9 过；全 suite 319/322——3 个 JsimBridge 基线失败（`Invokedynamic…`/`Athrow…`/`IndyMethodFailsClosedAtBridge`）批前即挂，与本批无关，其余 pass 无隐含 DeadCode no-op 依赖暴露。
- 罐 A 单 pass（raw jar，`GRUNT_TRACE=1 GRUNT_SLICE_SEC=0`）：scan methods=0（修复前后同）——v3 全链 raw 罐无可达性死区，死块由上游裁决产生（PredicateFold NOT-TAKEN 臂 / GateProcessorFold junk 臂），本 pass 设计运行点在上游之后；双跑 3088 条目 CRC 全等、类数 3074 不增、`-Xverify:all` 失败集与输入全等（2847 ok / 227 NCDFE，VerifyError=0）。
- 罐 A 配对链补测（`grunt.PredicateFold,grunt.DeadCode` 与 `grunt.GateProcessorFold,grunt.DeadCode`，双跑）：配对上游照常工作（GateProcessorFold 7643/7643 同台账），但 jar A raw 罐上 DeadCode scan 仍 methods=0——罐 A 的折叠形不留不可达延续段，语料级吸附/删表分桶均为 0；吸附/删表行为面以单测三用例为证。

## JSR/RET

`JSR` 的后继 = 目标 + 顺序下一条（返回地址压栈）；`RET` 无静态后继（目标在局部变量里）——按终止符处理，保守不越。Strict 帧重算对 jsr/ret 抛异常，`mutateMethodWithFrames` 收敛为 false（整方法放弃）。

## 与 HotSpot 线性 walk 的关系

HotSpot 校验器对死代码并非「跳过」：线性 walk 走遍全部帧，且每个分支目标 / 异常 handler 都必须有帧。所以死区必须在导出前物理消失，而不是留 NOP 或留帧——本残留擦掉死区，帧由 `rewireFramesAfterLower` 的 Strict 求解从代码真值整体重算，不存在「锚在被擦指令上的孤儿帧」这类问题（手剪帧的 `smt_tools::pruneOrphanFrames` 已随 smt_toolbox.hpp 一起删除）。
