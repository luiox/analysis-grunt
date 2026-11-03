# proxy_inline 实现笔记

## 数据流

`Site{callerUri/Owner/Method/Desc, callerMethodIndex, callSrcPc, proxy 三元组,
target 三元组, virtualTarget}` → `DirtySet{uris, attempted, applied,
failedByReason, skippedByRoundTrip, proxies}`。`callSrcPc` 是原始字节布局下的
pc（跨会话稳定），与其他 invoke 面残留同一套锚定方式。

## Scan

两遍：

1. **定义侧（buildDefs）**：逐方法 promote，先 `shapePlausible` 粗筛（末条
   是 `*return`、倒数第二条是 invokestatic/invokevirtual——沾形状才进拒绝
   面，其余静默跳过不刷屏），再 `decodeProxyBody` 精查：
   - 直线门：任何指令带标签 / 体带异常表 → `BodyNotProxyShape`；
   - 实参区 = 描述符走槽（long/double 占两槽）逐槽比对：`loadMatches` 同时
     认带操作数的 `xload n` 与把槽位编进操作码的紧凑形态 `xload_0..3`
     （CodeDom 不做归一化，汇编器产出哪种就是哪种）；
   - 转发指令：static 目标要求代理描述符 == 目标描述符；virtual 目标要求
     首参 == `L目标owner;` 且余部 == 目标描述符，错配 → `TargetDescMismatch`；
     `invokespecial`/`invokeinterface` 被 shapePlausible 直接排除在形状外；
   - 返回指令与描述符返回类型吻合，错配 → `ReturnMismatch`；
   - `synchronized` 代理（内联丢锁）→ `ProxySynchronized`。
   定义表是 `owner\nname\ndesc → {targetOwner, targetName, targetDesc,
   virtualTarget}`。

2. **调用点普查（collectSites）**：全容器逐 `INVOKESTATIC` 查定义表，命中
   即站点；**按代理分组**后过初始化暴露门——存在 owner 外调用点的代理，
   owner `<clinit>` 必须中性（缺失或只剩 NOP/RETURN，与 constant_call 同
   口径），非中性**整组放弃**（只放一半调用点会把初始化时机劈成两半），
   记 `OwnerInitExposed`。owner 级中性判定带缓存，一类只判一次。零引用
   代理不产站点、不刷拒绝（死码归 DeadMember）。

## Rewrite

- 按 uri → methodIndex 两级聚合；前置门 `roundTripSafe`（往返不了的方法
  不进改写面）。
- `rewriteSite`：按 `srcPc+op` 定位调用指令，**复核成员引用仍是该代理**
  （防同方法前站改写串位），然后原地换：op 换 `INVOKESTATIC`/`INVOKEVIRTUAL`
  （按 `virtualTarget`），`setMember` 换成目标方法引用（CP intern 去重，只
  追加），`srcPc` 保——锚在调用指令上的标签/调试行不动。`setMember` 失败
  把整条指令还原（失败不留半截）。
- **栈效应不变**：恒等转发保证弹栈/压栈集合一致，栈帧无需修补；lower 走
  常规链（prepare → lower），不改帧纪律。
- **不动代理本体**：内联后代理变零引用死码，交给下游 DeadMember/DeadCode
  ——链序上本残留排在其前，同一轮就能清掉。
- lower 失败：粒度 = 类，整类放弃回写。

## Proof

`proof_tools::classesIntact`：类数不增 + 脏类可重新解析。与全部残留同门。

## 已知边界

- 套娃代理单轮只折一跳：rewrite 只消费 ScanResult，转发目标若也是代理，
  下一轮整链重复时再折（README 收敛口径）。
- 伪 `bridge`/`synthetic` 标志不摘（flags_strip 的业务）；摘干净后 DeadMember
  的 ACC_BRIDGE 豁免不再误保这些死代理体。
- 转发目标可解析性不在本残留验：目标可以是容器外方法（JDK / 依赖库），
  改写只动调用点的方法引用，解析交给加载期。
- owner 中性门复用了 constant_call 的 `classInitNeutral` 口径（当前各自
  文件内实现），收敛进扫描工具箱是下批的事。
