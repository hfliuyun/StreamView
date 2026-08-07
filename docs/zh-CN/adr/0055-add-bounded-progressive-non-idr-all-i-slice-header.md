# 新增有界 progressive non-IDR all-I slice header

状态：已接受
日期：2026-08-07

## 背景

内置 H.264 规则现在可以解码有界 progressive all-I IDR slice header，包括 PPS 控制的
picture-order、redundant-picture 与 deblocking 分支。NAL unit type 1 仍保持 opaque payload。
non-IDR all-I header 与它共享 SPS/PPS 依赖的 frame 与 picture-order 字段，但没有
`idr_pic_id` 和 IDR reference-picture-marking flag。

完整 non-IDR 语法在 `nal_ref_idc` 非零时还会出现 reference-picture marking。该分支尚未建模，
而 payload dispatch 只按 NAL type 选择，不能把 reference-marking bit 静默当作下一个字段读取。
因此下一增量需要显式、带 source 位置的 prerequisite。

## 决策

在 direct header 中新增第二条 assertion：

```cpp
assert(nal_unit_type != 1 || nal_ref_idc == 0) at nal_ref_idc;
```

这把首个 type-1 payload slice 限定为 non-reference picture。`nal_ref_idc` 非零的 type-1 NAL
在完整 header 解码后、EBSP/RBSP payload mapping 前失败；header 保留为 partial result，后续 NAL
继续扫描。

新增 `NonIdrAllISliceLayerWithoutPartitioningRbsp`，并把 NAL type 1 派发给它。该 structure
只接受 `slice_type` 值 2 和 7，沿用现有 progressive SPS 与 POC-type-0 dynamic-width
prerequisite，导入精确 PPS/SPS generation，并按 clause 顺序读取：

- `first_mb_in_slice`、`slice_type`、`pic_parameter_set_id`、`frame_num`、`pic_order_cnt_lsb`；
- PPS 控制的 `delta_pic_order_cnt_bottom` 与 `redundant_pic_cnt`；
- `slice_qp_delta` 与 PPS 控制的 deblocking-filter control；以及
- 覆盖所有剩余 RBSP bit 的 opaque `compressed_payload slice_data`。

IDR 专属的 `idr_pic_id`、`no_output_of_prior_pics_flag` 与 `long_term_reference_flag` 不出现。
由于 direct assertion 拒绝非零 reference priority，该 structure 不声称解析
`dec_ref_pic_marking`。

package version `0.1.12` 以 coverage depth `all-i-slice-header` 发布这一 additive rule change。
回归覆盖合法 type-1 all-I header、精确字段/payload span、不支持的非零 reference 边界，以及
后续 NAL 继续扫描。既有 opaque-fixture NAL 改用 type 12，继续覆盖未派发 payload 路径。

## 影响

首个 non-IDR slice header 现在可以带 source mapping 并可恢复，同时不假装解码
reference-list 或 picture-management 语法。两个有界 all-I shape 共用同一 PPS/SPS generation 与
mapped-payload machinery；不支持的 reference picture 会在 payload 被误读前失败。

## 非目标

本决策不新增 P/B/SP/SI slice type、非零 reference 的 non-IDR header、field picture、POC type 1/2、
reference-list modification、weighted prediction、adaptive memory-management operation、slice
group、partitioned data 或 CAVLC/CABAC 解码；不新增 header-context publication mechanism，也不改变
opaque NAL type 的语义。
