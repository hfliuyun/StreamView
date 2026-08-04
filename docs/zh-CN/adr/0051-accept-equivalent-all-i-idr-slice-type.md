# 接受等价的 all-I IDR slice type

状态：已接受
日期：2026-08-04

## 背景

首个有界 IDR slice-header rule 接受 progressive all-I 形式 `slice_type == 2`。
H.264 还允许等价的 all-I 值 7。这两个值的 header layout 完全相同；差异只是 coded
value alias，不会新增 reference-list、field-picture 或 prediction branch。根据 ADR-0050，
DSL 现在可以为 `ue` 字段提供命名 enum 值域。

## 决策

在 bundled H.264 rule 中声明 `IdrAllISliceType { i = 2; all_i = 7; }`，并为 `slice_type`
标注 `@enum(IdrAllISliceType)`。NAL unit type 5 继续使用同一个
`IdrSliceLayerWithoutPartitioningRbsp` structure、dynamic imported width、IDR marking flag、
`slice_qp_delta` 与剩余 bit opaque payload。值 2 和 7 都作为带 enum type name 的无符号 `u64`
字段物化；其他值在完整 `slice_type` Exp-Golomb 码字位置产生致命 `invalid-syntax`。当前
NAL 局部失败，后续 NAL 继续扫描。

package 版本升到 `0.1.9`，coverage token 保持 `idr-slice-header`。type-7 回归 fixture 验证
7-bit 码字和精确剩余 11-bit opaque suffix；非法 type-3 fixture 验证绝对 bit 33 处的 5-bit
source span diagnostic，以及后续仍然 materialized 的 AUD。

## 影响

使用任一规范 coded value 的常见 progressive IDR all-I slice 现在共享同一个 source-mapped
header projection。rule 仍保持 format-neutral，不会根据 enum member 名称推断额外 slice syntax。

阶段 3 的 slice-header 目标仍未完成：bottom-field POC、redundant picture、deblocking control
的 imported-context conditional branch，以及之后的 non-IDR/P/B reference-list 和
weighted-prediction branch 仍需独立增量。

## 非目标

本决策不支持 `slice_type` 值 0、1、3、4、5、6，不支持 non-IDR NAL、P/B/SP/SI 语法、field
picture、POC type 1/2、reference-list modification、weighted prediction、adaptive
memory-management operation 或 CAVLC/CABAC 解码。
