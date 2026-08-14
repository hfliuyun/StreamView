# 按码流位置绑定重定义的参数集

状态：已接受
日期：2026-08-14

## 背景

ITU-T H.264 clause 7.4.1.2 允许码流在后续位置重定义拥有相同 `seq_parameter_set_id`
或 `pic_parameter_set_id` 的序列参数集（SPS）或图像参数集（PPS）。在重定义点之后解码
的 slice 激活新发布的参数集，而在重定义点之前的 slice 必须保持绑定至先前的 generation。

ADR-0028 在 `ContextDirectory` 中确立了按位置解析的设计，ADR-0044 与 ADR-0045 在
DSL 运行期中集成了声明式 `@context_export` 与 `@context_import` 生命周期，ADR-0073
验证了 PPS 扩展根据流位置查找活跃 SPS generation，不会错误使用未来或过期的 generation。

然而，在 Annex B analyzer 测试套件中，尚未通过端到端回归 fixture 正式锁定跨中途重定义
的 slice header 绑定行为（包括由上下文动态计算的字段宽度，如根据
`log2_max_frame_num_minus4 + 4` 计算的 `frame_num` 宽度）。

## 决策

在内置 H.264 analyzer 中正式锁定流中参数集中途重定义与按位置 generation 绑定的端到端
验收：

1. **按位置激活 generation**：当 SPS 被重定义（例如将 `log2_max_frame_num_minus4`
   从 0 修改为 2）并经由随后的 PPS 重新绑定后，后续 slice header 能够动态根据新发布的
   generation 计算上下文表达式，产生拓宽后的字段（例如 `frame_num` 从 4 bit 拓宽为
   6 bit），不产生语法错误或位偏移漂移。
2. **先前 generation 稳定性**：在重定义之前完成分析的 slice 保持绑定到最初的
   generation，保留其已物化的结构、精确的字段 bit 长度与零诊断状态。
3. **非法重定义隔离**：非法或格式错误的参数集 NAL（例如带有保留 profile IDC 的 SPS）
   进入 `invalid` 状态，不会发布新 generation 亦不会污染活跃 generation 表。引用该
   非法定义的参数集与 slice 产生 `dependency-unavailable` 诊断，而先前的有效 slice
   完全不受影响，保持完整物化。

## 影响

端到端回归测试验证：

- 正向用例：`SPS(id 0, log2_max_frame_num_minus4=0)` → `PPS(id 0)` →
  `Slice A (frame_num=4 bits)` → `SPS(id 0, log2_max_frame_num_minus4=2)` →
  `PPS(id 0)` → `Slice B (frame_num=6 bits)` → 后续 `AUD`，断言两个 slice 均以精确且
  不同的动态宽度解码，并具有完整的有序子节点列表；
- 负向用例：`SPS(id 0, valid)` → `PPS(id 0)` → `Slice A` →
  `SPS(id 1, invalid profile)` → `PPS(id 1)` → `Slice B` → 后续 `AUD`，断言 Slice A
  保持完全有效且物化，而 Slice B 产生 `dependency-unavailable` 诊断。

阶段 3 第 5 项（「支持同 ID SPS/PPS 中途重定义和按位置选择」）得到满足。

## 非目标

本决策不实现动态解码图像缓冲（DPB）管理、IDR 序列重置、恢复点同步或多 slice group
重定义。

## 后续

- ADR-0028：按源码位置解析上下文版本
- ADR-0044：发布规则声明的上下文代
- ADR-0045：导入规则声明的上下文代
- ADR-0073：解码有界 High Profile PPS 扩展
