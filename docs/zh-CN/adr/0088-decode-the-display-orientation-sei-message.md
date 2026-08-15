# ADR-0088：解析显示方向 SEI 消息

- **状态**：已接受 (Accepted)
- **日期**：2026-08-15
- **决策者**：StreamView 核心团队

## 背景 (Context)

H.264 Annex B 官方规则包（`org.streamview.h264`）已支持解析多种 SEI 消息负载类型（如 `buffering_period` (0)、`user_data_registered_itu_t_t35` (4)、`user_data_unregistered` (5)、`recovery_point` (6) 以及 `frame_packing_arrangement` (45)）。其他 SEI 负载类型目前暂由回退分支 `@lazy(payload_size) bytes payload_data` 处理。

**显示方向 SEI 消息**（Display Orientation SEI message，`payload_type == 47`，ITU-T H.264 D.1.27 及 D.2.27 节）用于向解码器和渲染器提供解码后图像在显示前所需的几何变换信息（如水平镜像翻转、垂直镜像翻转及逆时针旋转角度）。

由于显示方向消息的所有语法元素完全自包含于其载荷内，不依赖外部 SPS 或 PPS 参数集上下文，因而可直接使用标准 DSL 定宽位域整数与条件分支语句进行结构化解码。

## 决策 (Decision)

我们在官方 H.264 规则包中实现显示方向 SEI 消息（`payload_type == 47`）的完整结构化解码，并将规则包版本升级至 `0.1.37`。

### 1. 语法映射

依据 ITU-T H.264 D.1.27 与 D.2.27 节规范：

- `display_orientation_cancel_flag`：`bits<1>`（指示本消息是否取消之前任何显示方向 SEI 消息的持续有效性）。
- 当 `display_orientation_cancel_flag == 0` 时：
  - `hor_flip`：`bits<1>`（指示目标显示是否需要水平镜像翻转）。
  - `ver_flip`：`bits<1>`（指示目标显示是否需要垂直镜像翻转）。
  - `anticlockwise_rotation`：`bits<16>`（指定目标显示的逆时针旋转角度，以 $2^{-16}$ 度为单位，表示范围 $0^\circ$ 至 $360^\circ - 2^{-16\circ}$）。
  - `display_orientation_repetition_period`：`ue @range(0, 16384)`（指定显示方向 SEI 消息的重复有效周期）。
  - `display_orientation_extension_flag`：`bits<1> @range(0, 0)`（保留扩展标志位，规范要求为 0）。
- `rbsp_trailing_bits`：标准字节对齐尾部比特。

### 2. 节点层级与命名

所有语法元素作为 `SeiRbsp` 在 `switch (payload_type)` 中 `case 47:` 分支的直接子节点物化。字段命名与 ITU-T H.264 规范语法定义完全一致。

### 3. 错误处理与隔离

- 任何载荷截断均触发分析器安全回滚并生成诊断告警，同时保障后续 NAL 单元能继续正常解析。
- 超出 `@range` 约束的值生成标准非致命语法校验诊断。

## 影响 (Consequences)

- 官方 `org.streamview.h264` 规则包版本由 `0.1.36` 升级至 `0.1.37`。
- 显示方向 SEI 消息获得逐 bit 精确坐标与完整的结构化检查支持。

## 参考 (References)

- ITU-T H.264 规范 D.1.27、D.2.27 节
- ADR-0087：解析帧封装排列 SEI 消息
