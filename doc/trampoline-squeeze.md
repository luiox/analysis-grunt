# trampoline_squeeze：跳到下一条的 goto 蹦床

## 形态

grunt 打乱基本块布局后，用「跳到布局下一条」的 goto 缝合控制流：

```text
  iconst_1
  goto +0          ; 目标就是物理下一条
  iconst_2         ; 实际后继
```

这类蹦床不表达任何语义，但把「紧邻操作数区」切断了：下游残留
（predicate_fold / constant_call）按「消费点往前紧邻收集」找操作数，蹦床
夹在中间就整站看不到。

## 判据（S2）

`goto/goto_w` 的目标（句柄相等）== 布局顺序后继。与字节偏移无关，改写后
跨会话仍稳定。

## 改写

原地置 NOP、保 `srcPc`——不擦除、布局不缩短，SMT/属性面零扰动。NOP 落点
由 `const_args::collectArgs` 跳过，被压住的操作数区重新可见。

## 证明

`classesIntact`：类数不增 + 脏类可重新解析。
