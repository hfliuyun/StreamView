# 解码 Picture-Order-Count Type 1 与 2

Status: Accepted
Date: 2026-08-11

## 背景

在本增量之前，内置 H.264 规则只解码 `pic_order_cnt_type == 0` 的 SPS 与 slice-header
picture-order syntax。SPS 用 `@equals(0)` 拒绝其他 type，两个 slice structure 还把
这条 prerequisite 编码在 `pic_order_cnt_lsb` 的 dynamic width 除数中。Baseline、Main
与 High 码流也可以使用 type 1/2，因此它当时是 slice header 中最宽的剩余
syntax 边界。

现有 DSL 已能表达全部必要分支：bounded repeat 可投射 type-1 SPS offset cycle，
`optional_value` 可归一化只在单个 SPS 分支存在的字段，imported equality guard
可选择 POC type，computed repeat count 可保留规范名称
`delta_pic_order_cnt[0]` / `[1]`。本增量不需要新的 parser、IR、VM 或 analyzer 能力。

## 决策

把 SPS 的 `@equals(0)` 替换为只包含 0、1、2 的闭集 `PicOrderCntType` enum。
reserved 值会选中一个未定义布局，因此仍在完整 Exp-Golomb codeword 上以致命
`invalid-syntax` 失败。

SPS 在 `pic_order_cnt_type` 之后选择互斥分支：

- type 0 读取已有的 ranged `log2_max_pic_order_cnt_lsb_minus4`；
- type 1 读取 `delta_pic_order_always_zero_flag`、两个 signed offset、ranged
  `num_ref_frames_in_pic_order_cnt_cycle`，以及最多 255 个投射后的
  `offset_for_ref_frame[index]`；
- type 2 不读取额外 POC 字段。

count 同时携带非致命 `@range(0, 255)` diagnostic 并驱动 `repeat(..., 255)`。
大于 255 的值会保留带 source 的 warning，然后在未声明 cycle entry 改变 SPS 后续布局前失败。

conditional source field 不能作为 context export，因此 SPS 分支之后用两个无条件
computed value 归一化：`effective_log2_max_pic_order_cnt_lsb_minus4` 在 type 0 之外
fallback 到零，`effective_delta_pic_order_always_zero_flag` 在 type 1 之外 fallback
到一。slice guard 保证两个 fallback 都不会被用来读取错误 POC 分支的字段。

IDR 与 non-IDR slice 都保留已有 type-0 字段顺序。effective always-zero flag 为一时，
type 1 不读取 delta；否则 computed count 为一，并在 PPS 启用第二个 delta 且 slice
编码帧图像时再加一。最多两项的 bounded repeat 随后发布
`delta_pic_order_cnt[0]` 以及可选的 `[1]`。type 2 在下一个 slice-header 字段之前不读取 POC syntax。

## 影响

package `0.1.24` 发布 coverage depth `picture-order-count-slice-header`。type-0 码流
保留已有 slice 字段顺序。SPS presentation 增加两个 computed normalization node，
显式携带 delta 的 type-1 slice 增加一个 computed count node。

测试覆盖三个 SPS 分支、type-1 offset cycle（包括空 cycle）、always-zero 与显式一/两个
delta slice、场图像对第二个 delta 的抑制、type-2 IDR/non-IDR 字段缺席、reserved POC type、
count 越界、截断、精确 source span，以及失败 NAL 之后继续扫描。

## 非目标

本决策不派生 `PicOrderCnt`、`TopFieldOrderCnt` 或 `BottomFieldOrderCnt`；不跟踪
`FrameNumOffset`、wrap state、MMCO 5、field pair 或 output order；不校验 offset sum 或
signed value domain；也不引入 decoded-picture buffer。这些属于 ordering/DPB 语义，而非
clause 7.3 syntax，继续延期。
