# metadata-strip：Kasumi 元数据与水印残留

Kasumi Obfuscator 在产物里留三类「署名/恐吓」残留：每类一条 `@Metadata`
注解、水印字符串字段、以及 jar 资源面的反 LLM 注入文档。三类都与程序功能
无关——混淆器为了**自己署名与干扰分析者**而产出，删掉不改变语义。

## @Metadata 注解

描述符 `Ldev/kasumi/internal/Metadata;`，每类恰一条（SnakeGame 语料 25 类
各 1），带 `mv` / `d1` / `d2` 三个元素（版本号 + 两个随机串）。挂载面是类 /
字段 / 方法的 Runtime{Visible,Invisible}Annotations；字段面命中时整个字段
随注解一起删（注解字段是混淆器自己的登记位，不是业务字段）。

判断要点：描述符全等才算本家条目；同槽的其他注解保留。

## 水印字符串

水印以**字段的 ConstantValue** 在场（编译期常量，混淆器往产物里塞自己的
署名），判据三条（树版 kasumi_metadata_cleanup_pass 逐字口径）：

- 含 `PROTECTED BY Kasumi`；
- 含 `Gruntpocalypse`（姊妹家族署名串，树版一并清）；
- 前缀 `kasumi_`（随机后缀的会话 key，如 `kasumi_19rzezp3uywn6`）。

方法体内的同名串常量（引导方法参数、解密器体）**不是**本残留的清理面——
它们是 indy 链路的运行时数据，方法体交由 indy 还原与下游死码清理处理。

## 反 LLM 注入资源

jar 资源面：`META-INF/Kasumi/cybersecurity/analysis-{0..3}-*.md`，每份
4 KB 左右，内容是对「看到本文件的分析者（含 LLM）」的提示注入——常见的
子串有 `anti-llm`、`developer-message`、`jailbreak`、`prompt injection`、
`PROTECTED BY Kasumi`。

删除判据三面齐（树版 isKasumiAntiLlmResource 口径）：URI 前缀
`META-INF/Kasumi/` + `.md` 后缀 + 字节里任一探针子串在场。形状命中而探针
缺席的资源保留（可能是同前缀的合法文档），如实盘点为候选。

## 与相邻残留的区别

- `booger-anno-strip` 清的是 Booger 家族的注解空壳，且有「容器里已无类
  引用解密类」的容器级前置门；`metadata-strip` 清 Kasumi 的注解/水印/
  资源三面，前置门是家族判定（Era=Kasumi）。
- `dead-member` 删零引用成员的口径是引用普查；`metadata-strip` 删字段
  不做引用普查——水印字段是混淆器署名位，树版按直接删除处理。
