# 将有界 sentinel repeat 降低为 guarded projection

Status: Accepted
Date: 2026-08-04

## 背景

有界 H.264 slice header 需要解码 reference-picture list modification 和
memory-management control operation 一类列表。每个列表至少读取一项，终止 operation 本身也是
需要展示的 syntax；完成一项后，如果其中一个 unsigned field 等于 sentinel，列表才结束。

既有 count-controlled `repeat (count, maximum)` 无法表达这种结构。通用 `while`、`break`、
EOF termination 或 mutable loop state 又会削弱 DSL 的静态 projection、确定性 bytecode 和有界
materialization 模型；这里需要的语法可以保持得更窄。

## 决策

新增一种 post-tested sentinel form：

```cpp
repeat (64) {
    ue modification_of_pic_nums_idc;
    if (modification_of_pic_nums_idc == 0) {
        ue abs_diff_pic_num_minus1;
    }
} until (modification_of_pic_nums_idc == 3);
```

`repeat` header 中的正整数字面量是最多完成的 iteration 数，范围固定为 `1..64`。`until`
clause 必须紧跟 body，只接受 `sentinel_field == integer`。sentinel field 必须直接声明在该 body
中，且是无条件、顶层、非数组的 source scalar。首个切片接受 fixed-width `bits`、enum 和 `ue`；
不接受 `se`、computed field、lazy region、dynamic-width field、nested control flow 中的字段或
body 外声明的字段。integer 必须能由 fixed-width field 或受支持的 `ue` domain 表示。

本切片也允许 equality condition 与 switch 使用此前已解码的 scalar `ue` controller，以便根据
sentinel body 中的 operation code 选择后续字段。它只比较已经解码的 unsigned value，不引入
general condition expression。

body 至少执行一次。每个选中的 iteration 会完整执行后再检查 sentinel，因此 sentinel 后的字段
仍按它们自己的普通 guard 运行；终止 sentinel field 会保留在分析树中。命中终止值后，所有后续
projection 都会跳过，不访问 source、不创建 node、不求 expression，也不检查 constraint。不提供
`break`、`continue`、其他比较、expression sentinel 或 EOF form。

compiler 会先验证一次 body，再把 `body projection * maximum` 计入既有 99,999-field 上限，并按
既有 outer-to-inner `[index]` 名称精确投影 `maximum` 次。iteration zero 继承外层 guard；之后
每次 iteration 还要求此前全部 projected sentinel 都不等于终止值。repeat-local name 不会逸出
body；statement 之后的静态 alignment 为 unknown。

typed IR 记录按序排列的 projected sentinel field index、termination value、外层 guard、assertion
position 和 source range。compiler 会在全部 projected field 后发射一条
`assert-sentinel-terminated` instruction。VM 在读取 source 前验证完整 descriptor 与 projection
guard shape；错误 field index、type、condition、value、ordering、assertion position 或超过 64 个
projected sentinel 都属于 invalid runtime definition。

runtime assertion 在某个已选 sentinel 等于 termination value 时成功。若完成全部 `maximum`
iterations 仍未命中，则在最后一个 sentinel field 上返回 `invalid-syntax`，同时保留已物化的有界
prefix 和已经消费的 bit。truncation 与 source error 仍以当前 field 为事务边界；instruction、node、
cancellation、mapping 和 partial-result 行为继续沿用既有 projected field 语义。

本切片不会让 bundled H.264 rule 使用新语法，因此 package version 与 coverage token 不变。
slice dispatch 和最终 compressed remaining-bit payload 继续留给后续决策。

## 影响

规则获得 H.264 operation list 所需的精确 bounded post-tested 结构，同时无需加入 runtime loop 或
mutable control-flow VM。生成程序仍是线性的，所有可能的 field、guard、instruction 与 node 都在
执行前有界。

`64` 是首个 form 刻意采用的 language-wide maximum。未来格式若需要更大边界，必须新增决策，
不能静默扩大 compiler memory 与 runtime work。

## 非目标

本决策不新增 pre-tested loop、count expression、general condition、`break`/`continue`、EOF 或
remaining-bit termination、collection value、expression 中的 loop index、loop bound 中的
imported context value、compressed-payload terminal 或 H.264 slice dispatch。
