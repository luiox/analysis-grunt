# field_access_proxy 实现笔记

## Scan

三步（`proxy_inline` 同构）：

1. `buildDefs`：逐类逐 static 方法，`shapePlausible`（尾指令是 return、次尾
   是字段访问）粗筛后进 `decodeProxyBody` 精查——直线体（无标签、无异常
   表）+ `walkParams` 逐槽过手 + 中位指令 `memberOf` 解析为 Field 语义 +
   `expectedDesc` 互证（描述符必须恰为按该字段该有的形状）+ 返回指令匹配。
   成立者进 `ProxyIndex`（owner\nname\ndesc → 字段三元组 + FieldKind）。
2. `collectSites`：全容器扫 `invokestatic` 调用点，callee 命中定义表者记
   Site（`callSrcPc` 原始字节布局 pc，跨会话稳定），按代理分组。
3. 初始化暴露门：调用点越出 owner 类的代理，owner `<clinit>` 必须中性
   （`scan_tools::classInitNeutral`，owner 级缓存）；非中性整组放弃并记
   `OwnerInitExposed`——只放一半调用点会把初始化时机劈成两半。

拒绝面只记「沾形状但不成立」的方法（synchronized / 互证错配 / 体形错 /
返回错），完全不沾形状的静默跳过（trace 不刷屏）。

## Rewrite

两遍式（与 proxy_inline / const_method 同构）：先收动作清单（按类分组），
再逐类「开会话 → `roundTripSafe` 探针（未改字节）→ 逐方法 promote →
`rewriteSite` 原地换形 → lower → `writeClass` 回写」。

`rewriteSite`：按 `srcPc` 定位 INVOKESTATIC，复核成员引用等于代理键，
`dom.get` 拿槽，`insn->op = fieldOpOf(kind)` + `setMember(Kind::Field, ...)`
——身份与 srcPc 不变（锚上标签不必迁移），栈效应不变（getter 弹 0/1 参压
1 值、setter 弹参不压值，与代理一致），栈帧无需修补。改不动就整条还原。
任一方法 lower 失败整类放弃回写（粒度=类）。

代理本体不删——改回后零引用，交 DeadMember（gateB + 引用普查）。

## Proof

`classesIntact`：类数不增 + 脏类可重新解析。另加收敛哨兵测试：改写后的
容器再扫站点归零（代理体内是字段访问，不构成新的代理调用点）。

## 已知边界

- 字段 owner 在容器外（`System.out` 形态）：互证只用描述符与字段引用本身，
  不需要 owner 类在场；初始化门只钉代理 owner（容器内）。
- 零引用代理、外置 `C$FieldProxy` 类的整类回收是 DeadMember 的业务；本
  残留只动调用点。
- 收敛口径：单轮把 ScanResult 的站点清完即收敛；代理体内不再出现
  `invokestatic` 调用点，本轮产物不会给本残留造新站点。
