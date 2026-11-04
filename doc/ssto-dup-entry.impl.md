# SSTO 新增 verify 失败归因：同名重复条目的导入口径（非 pass 判据）

结论先行：SSTO-Core 全链输出的 3485 个 verify 失败类（对输入基线新增 1527）
**不是任何 grunt pass 的判据缺陷，也不是 verify 噪声**——是基座（mjbase 0.0.5）
容器导入口对「同名重复条目」的保留口径与 JVM 类加载的实际口径相反所致。
按红线 mjbase 0.0.5 不动，本文件挂账基座，取证全在 `E:/corpus-ws/ssto/`。

## 现象

- 输入基线（`java -Xverify:all VerifyJar`，6971 名）：3916 失败 = 3912
  `NoClassDefFoundError`（混淆器剥掉的依赖，原生挂）+ 4 `ClassFormatError`。
  即 JVM 对输入读到的字节基本是真实类。
- 全链输出（快照 ce8b4a7 与 master 同因同数，上批差分已证）：3486 名、3485
  失败，全部 `ClassFormatError: Incompatible magic value 0x32f9d4b4`；对基线
  新增 1527（corpus-regress 口径如实计账）。
- **单 pass（TrampolineSqueeze only）即复现同规模损坏**——与 13 pass 的改写
  判据全部无关（本 pass 只 NOP 蹦床 goto，写不出 31 字节垃圾内容）。

## 取证链

1. **罐体结构**（python zipfile 原始 CEN 视角）：SSTO 给被 Dup 的类名挂多条
   同面条目，CEN 序 = 2 字节垃圾 → 31 字节垃圾（magic `0x32f9d4b4`，
   decoy 内容）→ 真实类；全部条目的 CRC 字段被伪改成 `0xdeadbeef`。
   输入 10456 个 .class 条目 = 6971 名 + 3485 条同名垃圾 Dup。
2. **JVM 实际口径**（mini jar 对照实验，`mini/junkfirst.jar` vs
   `mini/realfirst.jar`，JDK 17.0.8 URLClassLoader）：同名重复条目
   **末现胜出**——垃圾在前、真实类在后的罐，`Class.forName` 读到真实类字节
   （wrong-name `NoClassDefFoundError`）；真实类在前、垃圾在后的罐读到垃圾
   （`ClassFormatError magic 855233716`）。两次都是末现生效。
3. **基座口径**：mjbase 导入口按「CEN 首现」保留类并警告
   `kept first occurrence, aligned with JVM first-wins class loading`
   （单罐 3485 条警告，与 Dup 数精确吻合）——首现恰是垃圾条目；真实类
   （末现）整条丢失，未进容器。导出罐里同名只剩垃圾条目（java ZipFile
   只见 3486 名；python CEN 视角可见 6971 条 = 3486 真实类名 + 3485 垃圾条目，
   逐名 probe 确认真实类条目消失）→ JVM 读到垃圾 → ClassFormatError。
4. **数字闭环**：3485 个 Dup 名全部中招；其中 1958 名在输入基线本就原生挂
   （缺依赖），故「新增」= 3485 − 1958 = 1527，与 corpus-regress 记账一致。

## 裁决

- **定性**：树版固有口径（基座语义与 JVM 实测不符），判据 bug 与 verify
  噪声均排除——ClassFormatError 是真实加载失败，工具链没有误报。
- **本仓动作**：不改（红线 mjbase 0.0.5 不动）；corpus-regress 的「新增
  失败」口径如实记账，账是真的（输出确实挂），只是责任面在基座。
- **基座挂账**：导入口的同名重复条目应按「CEN 末现」保留（与 JDK 17 实测
  的类加载口径对齐；`kept first occurrence` 警告文案同步改），或保末现类
  + 首现垃圾仍按资源保留。修复后 SSTO 这批新增失败应整批消失。

## 复现

`E:/corpus-ws/ssto/`：`SSTO-Core.jar`（ASCII 名副本）、`Probe.java`
（同名首视图逐条目 magic/CRC）、`Probe2.java`（指定名全条目序列）、
`mini/`（JVM 末现口径对照实验）、`p1.jar`（单 pass 复现产物）、
`full13.jar`（全链复现产物）。
