# 新增有界 P-slice CABAC initialization branch

状态：已接受
日期：2026-08-07

## 背景

ADR-0059 完成了 progressive non-reference P-slice header 的有界 list 0 modification
语法。bundled rule 随后通过 source-anchored assertion 要求所选 PPS 的
`entropy_coding_mode_flag` 为零。该 prerequisite 能保住 `slice_qp_delta` boundary，
但也会在读取条件字段 `cabac_init_idc` 之前拒绝所有 CABAC-coded P slice。

clause 7.3.3 在 entropy coding 已启用且 slice type 不是 I/SI 时，把 unsigned
Exp-Golomb `cabac_init_idc` 放在 weighted prediction 与 reference-picture marking
语法之后、`slice_qp_delta` 之前。当前 bundled profile 只接受 P/I 值，并要求 type-1
direct header 的 `nal_ref_idc == 0`。因此其精确条件是 P slice 且 imported PPS 的
`entropy_coding_mode_flag` 等于一；中间不存在 `dec_ref_pic_marking()` 分支。

PPS 已经 export `entropy_coding_mode_flag`，slice structure 也已经 import 该精确 PPS
generation。现有 nested imported equality guard 与 unsigned Exp-Golomb range constraint
足以表达这段语法，不需要扩展 parser、compiler、VM 或 context model。

## 决策

只移除要求 `entropy_coding_mode_flag == 0` 的 P-slice assertion。保留
weighted-prediction prerequisite，然后在 `slice_qp_delta` 之前通过嵌套 local/imported
guard 读取 `cabac_init_idc`：

```cpp
assert(!is_p_slice ||
       context_value(pic_parameter_set_id, h264_pps, weighted_pred_flag) == 0)
    at pic_parameter_set_id;

if (is_p_slice) {
    if (context_value(pic_parameter_set_id,
                      h264_pps,
                      entropy_coding_mode_flag) == 1) {
        ue cabac_init_idc @range(0, 2);
    }
}

se slice_qp_delta;
```

外层 guard 不可省略。即使所选 PPS 启用 entropy coding，I/all-I slice 也不包含
`cabac_init_idc`。local 或 imported guard 为 false 时不消费 source bit，也不发布字段。
编码 P 值 0/5 共用同一分支。

clause 7.4.3 将 `cabac_init_idc` 约束为 `0..2`。该值域不会选择后续任何 slice-header
字段布局，因此规则使用非致命 `@range(0, 2)`，而不是闭集 enum。大于二的值会保留完整
Exp-Golomb 字段及其精确 source span，附加既有 warning severity 的 `invalid-syntax`
diagnostic，并继续解析 `slice_qp_delta`、可选 deblocking 语法与 opaque `slice_data`
boundary。截断的 Exp-Golomb 码字仍是致命 source read failure，不发布 partial field。

既有 weighted-prediction assertion 保持在新分支之前，因为 bundled profile 仍未声明
`pred_weight_table()`。type-1 direct-header assertion 继续要求 `nal_ref_idc == 0`，因此
reference-picture marking 仍然缺席。

package version `0.1.16` 发布这一 additive field surface。coverage depth 保持
`i-p-slice-header`，因为该增量只完成现有 non-reference P-slice shape 的一个可选字段，
没有新增其他 slice family，也不解码 compressed data。

回归 fixture 覆盖 entropy-disabled absence 路径；entropy-enabled P 值 0/5；全部合法
`cabac_init_idc` 值；值 3 warning 且 QP/deblocking/payload boundary 不变；
entropy-enabled all-I slice 的字段缺席；截断码字；精确 child order 与 source span；以及
后续 NAL unit 继续扫描。

## 影响

bundled rule 现在可以公开受支持 non-reference P slice 选择的 CABAC initialization table，
同时保留精确 source mapping 与既有有界 slice-header projection。entropy-enabled I slice
仍能正确对齐，因为 local P-slice guard 会短路 imported condition。

超出范围的 initialization identifier 对分析者仍然可见，不会丢弃后续布局仍然确定的
header。这延续了既有 unsigned Exp-Golomb range constraint 对 framing 与 conformance 的
区分。

## 非目标

本决策不解码 CABAC/CAVLC `slice_data`，不初始化或校验 CABAC context table，不新增
weighted prediction 或 `pred_weight_table()`、B/SP/SI slice type、list 1 modification、
nonzero-reference type-1 header、reference-picture marking 或 adaptive memory-management
operation、field picture 或 MBAFF、POC type 1/2、slice group 或 partitioned data，也不修改
opaque payload 语义；不扩展 DSL、context model、compiler、VM，也不改变 `@range` 与 enum
constraint 的 diagnostic severity。
