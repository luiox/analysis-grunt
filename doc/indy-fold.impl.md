# indy_fold 实现笔记

## 数据流

`Site{uri, methodIndex, insnPc, bsm 三元组, args[], indyName/Desc, foldable,
target, why}` → `DirtySet{uris, sites, classes}`。`Site.insnPc` 是 **DOM
order 下标**（不同于其他残留的原始 pc 锚）——rewrite 按 `order[insnPc]`
定位后复核 op 再动。

## Scan

- 有 `BootstrapMethods` 属性的类才进面；逐方法 promote，逐条 `INVOKEDYNAMIC`
  建站点。CP 无效 / 非 InvokeDynamic / bootstrap 下标越界 / BSM 句柄解析
  失败都记 `why` 后保留站点（scan 产出**全部**调用点，便于观测失败面）。
- BSM 静态参数经 `readBsmArg` 收成 `BsmValue`（Str/Int/Class/MethodType/
  MethodHandle），收不出的标 `Unsupported`；个数与 BSM 描述符不符直接失败。
- **模拟执行**：`runBsm` 把 BSM 前三个形参（Lookup/name/MethodType）灌
  Unknown（「不知道」比「编一个」安全），静态参数灌实值，libjsim 跑到底，
  读返回的 `CallSite` 载荷里的 MethodHandle 与其 reference_kind。
- 裁决三条（`judge`）：句柄载荷完整；句柄描述符 == 调用点描述符（否则
  `asType` 做过适配，换指令语义会变）；reference_kind ∈ {invokeVirtual(5),
  invokeStatic(6)}。任一不成立记 `why`，不猜。
- 模拟失败（跑不动 / 不是 ConstantCallSite / 读不出句柄）计 `bsmFailed`。
  BoogerV2 语料实测 foldable=0：BSM 形态不在纯目标子集内——本残留判据
  面向换代样本，验收方式是「构造正例单测 + 语料不新增失败」。

## Rewrite

- 按 uri → methodIndex 两级聚合；只动 `foldable` 站点。
- 定位 `order[insnPc]` 后**复核 op 仍是 INVOKEDYNAMIC**（同方法前站改写
  不位移 DOM 下标，但防御性复核不亏）。
- 就地换指令：`invokestatic`（Static）或 `invokevirtual`（Virtual），
  `setMember` 换成目标方法引用（CP intern 去重，只追加）；换不动则把 op
  还原成 INVOKEDYNAMIC——失败不留半截指令。
- `lowerMethod` 失败：粒度 = 类，整类放弃回写。无删除、无插入，pc 布局
  不变，调试属性与栈帧无需重定位。
- **不压缩常量池**：`BootstrapMethods` 仍引用 BSM 句柄与密文参数，压缩会
  把活条目当死数据搬走。清属性是另一个残留的事。

## Proof

`proof_tools::classesIntact`：类数不增 + 脏类可重新解析。与全部残留同门。

## 已知边界

- `Special`(7)/`Interface`(9) 句柄不折：换指令要动栈语义（`this` 槽），
  收益配不上风险。
- 非 `ConstantCallSite`（可变调用点 / volatile）不折：目标可能被运行期
  换掉，静态改写不安全。
- BSM 引用容器内解密方法时，先跑整链前序（ConstantCall 折解密体）不改变
  本残留结论——BSM 模拟自带 str/num/reflect/indy 扩展，解密在模拟里直接
  算出来，不依赖调用点侧折叠。
- 单轮不迭代：改写产出的普通 invoke 若又被混淆器包 indy（不存在此生成
  面），靠整链重复。
