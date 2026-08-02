# 增加有界 H.264 HRD Parameters

Status: Accepted
Date: 2026-08-03

## 背景

内置 H.264 VUI core 当前要求两个 HRD presence flag 都等于零。Annex E.1.2 把每个 HRD
分支定义为有界 schedule：一个 `cpb_cnt_minus1`、两个 scale 字段、一至 32 个 CPB schedule
entry，以及四个 delay-length 字段。任一 NAL 或 VCL HRD 分支存在时，Annex E.1.1 随后还会
包含 `low_delay_hrd_flag`。

稳定 DSL 可以用 computed count 与有界 `repeat` 表达该 schedule，但不能从一个结构调用另一个
结构，而且一个结构所有分支中的字段名都必须唯一。因此两个可能存在的 HRD instance 必须用
不同字段名 inline 表达，不能在 analyzer 中增加 H.264 特判。

## 决策

移除 NAL 与 VCL HRD presence flag 上的 `@equals(0)`。每个存在的分支都使用 `nal_hrd_` 或
`vcl_hrd_` 字段前缀 inline 解码完整 Annex E.1.2 语法：

- `cpb_cnt_minus1`、`bit_rate_scale` 与 `cpb_size_scale`；
- 重复的 `bit_rate_value_minus1`、`cpb_size_value_minus1` 与 `cbr_flag` schedule entry；
- `initial_cpb_removal_delay_length_minus1`、`cpb_removal_delay_length_minus1`、
  `dpb_output_delay_length_minus1` 与 `time_offset_length`。

为两个 `cpb_cnt_minus1` 增加非致命 `@range(0, 31)`。把 schedule count 计算为
`cpb_cnt_minus1 + 1`，并用作 `repeat(..., 32)` controller。因此大于 31 的值先在已解码 count
上保留带 source 位置的 warning，再因 layout-critical repeat bound 超限而在进入 schedule
entry 前停止。此边界前两个 scale 字段与 computed count 可能已经 materialized；不会消费任何
schedule entry。

两个可选 HRD 分支之后，materialize 一个 computed Boolean，任一 presence flag 为一时其值为
true。它只用于有条件地解码 source-backed `low_delay_hrd_flag`；随后既有
`pic_struct_present_flag`、bitstream-restriction 分支和 SPS trailing bits 保持不变。

package coverage token 保持 `parameter-sets`，增加的语法以 package `0.1.6` 发布。

## 影响

NAL HRD、VCL HRD 以及同时携带两个分支的 stream 都能以有界 schedule projection 和精确
source span 展示。重复字段使用与 `SchedSelIdx` 一致的零起点 materialized index。派生的
schedule count 与 combined-presence Boolean 是没有 source location 的可见 computed field；
所有 H.264 syntax field 仍然 source-backed。

越界 CPB count 会产生本切片所需的两类证据：字段保留非致命 value-domain warning，同时
repeat 边界阻止 unsupported count 改变已声明布局。一个 SPS 失败仍只影响自身，不妨碍扫描
后续 NAL unit。

## 非目标

本决策不解析 buffering-period 或 picture-timing SEI，不使用 HRD 值解释后续 timing 语法，
也不把 SPS 注册到 context directory。除已声明 count bound 外，不声称 bitrate、CPB size、
delay length 或 schedule relation 满足依赖 level 的 conformance。它不增加可复用结构调用、
fatal range annotation，也不在 Annex B analyzer 中增加新的 H.264 行为。
