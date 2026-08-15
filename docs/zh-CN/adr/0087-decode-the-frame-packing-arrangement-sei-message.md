# ADR-0087: 解码帧打包排列 SEI 消息

- **状态**: 已接受 (Accepted)
- **日期**: 2026-08-15
- **决策者**: StreamView 核心团队

## 背景

H.264 Annex B 官方规则包（`org.streamview.h264`）已支持多种 SEI 消息载荷类型的结构化解码（如 `buffering_period` (0)、`user_data_registered_itu_t_t35` (4)、`user_data_unregistered` (5) 与 `recovery_point` (6)）。其余 SEI 载荷类型此前由 `@lazy(payload_size) bytes payload_data` 回退分支承接。

**帧打包排列 SEI 消息**（Frame Packing Arrangement SEI, `payload_type == 45`，ITU-T H.264 条款 D.1.25 与 D.2.25）用于向解码器提供立体 3D 视频帧排列信息（例如左右并排 side-by-side、上下排列 top-and-bottom、棋盘格 checkerboard 或顺序帧 frame sequential 等排布模式）。

由于该 SEI 消息的所有语法元素均完全自包含于其自身载荷内部，不依赖外部 SPS 或 PPS 上下文，因此可以直接基于既有 DSL 整数类型、条件语句与计算布尔表达式进行结构化解析。

## 决策

在官方 H.264 规则包中实现帧打包排列 SEI 消息（`payload_type == 45`）的全字段结构化解码，并将规则包版本升级至 `0.1.36`。

### 1. 语法映射

严格依据 ITU-T H.264 条款 D.1.25 与 D.2.25：

- `frame_packing_arrangement_id`: `ue`（无符号 Exp-Golomb 整数，标识消息用途）；
- `frame_packing_arrangement_cancel_flag`: `bits<1>`（指示是否取消此前帧打包排列消息的持续性）；
- 当 `frame_packing_arrangement_cancel_flag == 0` 时：
  - `frame_packing_arrangement_type`: `bits<7> @range(0, 7)`（排布模式类型：0 棋盘格，1 列交错，2 行交错，3 并排，4 上下，5 顺序帧，6 2D 帧，7 瓦片）；
  - `quincunx_sampling_flag`: `bits<1>`（指示是否为梅花形采样）；
  - `content_interpretation_type`: `bits<6> @range(0, 2)`（0 未指定，1 帧 0 左/帧 1 右，2 帧 0 右/帧 1 左）；
  - `spatial_flipping_flag`: `bits<1>`（指示是否进行了空间翻转）；
  - `frame0_flipped_flag`: `bits<1>`（指示帧 0 是否翻转）；
  - `field_views_flag`: `bits<1>`（指示序列中是否均为场图像）；
  - `current_frame_is_frame0_flag`: `bits<1>`（指示当前解码图像是否对应帧 0）；
  - `frame0_self_contained_flag`: `bits<1>`（指示帧 0 是否自包含）；
  - `frame1_self_contained_flag`: `bits<1>`（指示帧 1 是否自包含）；
  - `has_grid_position`: `computed<bool> = quincunx_sampling_flag == 0 && frame_packing_arrangement_type != 5;`
    - 当 `has_grid_position` 为真时：
      - `frame0_grid_position_x`: `bits<4> @range(0, 15)`
      - `frame0_grid_position_y`: `bits<4> @range(0, 15)`
      - `frame1_grid_position_x`: `bits<4> @range(0, 15)`
      - `frame1_grid_position_y`: `bits<4> @range(0, 15)`
  - `frame_packing_arrangement_reserved_byte`: `bits<8> @range(0, 0)`（保留字节，标准约束为 0）；
  - `frame_packing_arrangement_repetition_period`: `ue @range(0, 16384)`（指定重复周期，范围 0..16384）；
- `frame_packing_arrangement_extension_flag`: `bits<1> @range(0, 0)`（扩展标志，标准约束为 0）；
- `rbsp_trailing_bits`: 标准 RBSP 字节对齐尾部比特。

### 2. 节点层级与命名

所有语法元素作为 `SeiRbsp` 的直接子节点在 `switch (payload_type)` 的 `case 45:` 分支中物化。字段命名与 ITU-T H.264 标准规范完全一致。

### 3. 错误处理与隔离

- 截断载荷触发分析器安全回溯并上报诊断警告，不影响后续 NAL 单元的连续解析；
- 字段取值超出 `@range` 范围时生成标准非致命验证诊断。

## 影响

- 官方 `org.streamview.h264` 规则包版本由 `0.1.35` 升至 `0.1.36`；
- 帧打包排列 SEI 消息获得比特级精度的源区间映射与结构化字段检查能力。

## 参考文献

- ITU-T H.264 条款 D.1.25, D.2.25
- ADR-0085: Decode the Buffering Period SEI Message
- ADR-0086: Ambient Context Imports and Active Parameter Set Resolution
