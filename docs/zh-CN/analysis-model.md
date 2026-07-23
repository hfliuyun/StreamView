# 分析节点与诊断模型

状态：阶段 1 实现基线。英文 [Analysis Node And Diagnostic Model](../analysis-model.md)
是规范性文档，本文件提供对应中文说明；如有歧义，以英文为准。

## 分析节点

分析树采用 append-only 结构，节点 ID 从 1 开始单调分配且永不复用。节点记录
kind、名称、物化状态、可选值、可选字段位置、子节点 ID 和附加诊断。展示元数据还可
携带类型名、项目编写的说明，以及由标准名和条款组成的规范引用。

节点 kind 包括 root、structure、syntax field、computed field、compressed
payload 和 region。Syntax field 必须具有精确字段位置；computed field 不能伪装
成具有 source 位置的字段。Root 不能作为子节点添加。空名称会被拒绝，且只有父
节点处于 `indexing` 时才能追加子节点。

树查询返回节点 snapshot；后续追加节点不会使既有 snapshot 失效，调用方也不会
持有指向可变树存储的裸指针。分析树由单个分析 worker 负责修改，线程安全发布与
UI model 更新由后续独立机制负责。

展示元数据与解码值和坐标一起复制到同一份稳定节点 snapshot 中，不改变解析或校验
语义。字段显示宽度由逻辑范围推导，不重复存储；绝对 source span 与逻辑视图/范围仍是
坐标的权威来源。

## Source Bit 定位

按 source bit 查询时，只考虑已经 materialized、并且字段位置的至少一个半开 source
span 包含该 bit 的节点。多个不连续 span 分别判断；落在间隙中的 bit 不匹配该节点。

查询结果具有确定性：首先选择树深度最大的匹配节点；深度相同时，选择所有 source
span 总覆盖长度更小的节点；覆盖长度仍相同时，选择稳定节点 ID 更小的节点。未物化
节点不会遮蔽已物化的父节点或同级节点。没有匹配节点时返回空 ID，原始 bit 选择可以
继续显示，同时清除分析树选择。

## 物化状态

核心明确区分：

- `lazy`：边界已知，但内容尚未请求；
- `indexing`：正在物化或索引该节点；
- `waiting-dependency`：等待已声明的分析依赖；
- `cancelled`：按请求停止，后续可以恢复为 `indexing`；
- `unsupported`：识别出语法，但超出声明支持范围；
- `invalid`：source 非法、截断、不可读，或超过经过检查的资源限制；
- `materialized`：声明范围内的内容已完成。

`unsupported`、`invalid` 和 `materialized` 是终态；`cancelled` 可以恢复到
`indexing`。活动状态可以进入终态、取消态或等待态。终态父节点不能再追加子节点。

## 诊断与部分结果

Parse diagnostic 包含稳定 code、severity、消息、字段路径和可选的精确字段位置。
首批 code 覆盖 source 截断、非法语法、不支持语法、取消、source I/O 错误、资源
限制和依赖不可用。

把节点标记为 cancelled、unsupported 或 invalid 时，只会附加诊断并改变该节点
状态；已经完成的节点、子节点、值和 source 位置全部保留。只要任意节点处于这三种
状态之一，树就报告存在部分结果。仅当所有节点均为 materialized 时，整棵树才算
完全物化。

非法示例包括：syntax field 没有位置、computed field 带 source 位置、把 root
作为子节点、向非 indexing 父节点追加内容，以及把 invalid 节点改回 indexing。

## 取消

Cancellation source 创建由共享状态支持的可复制 token；任意 source 或 token 仍在
引用时，该状态都会保持有效。请求取消是线程安全、无异常且幂等的。消费者只在文档
规定的取消点轮询 token；token 本身不会抛异常、强制终止工作或修改分析树。观察到
请求的 worker 负责发布已完成工作，并使用诊断把未完成节点标记为 cancelled。底层
存储机制属于实现细节，参见 [ADR-0018](adr/0018-portable-cancellation-state.md)。
