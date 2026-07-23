# StreamView 格式定义语言

状态：设计草案；下方的最小 0.1 子集已经接受。除此之外的功能在单独接受前仍是暂定设计。英文版是规范性来源：[Format Definition Language](../../format-language/README.md)。

StreamView 格式定义语言用于描述容器和编解码格式的语法，不执行不受限制的原生代码或通用脚本。内置规则和用户安装的规则使用同一套语言与运行时。

## 文档契约

一项语言功能只有在参考文档完整说明以下内容后，才可视为稳定：

- 完整语法和静态规则；
- 运行时语义和源坐标行为；
- 所有诊断及恢复行为；
- 资源限制和可取消位置；
- 兼容与弃用规则；
- 至少一个合法示例以及有关的非法示例。

只存在于 C++ 实现中、或者只在示例中出现的行为，不属于公开语言契约。

## 设计原则

- 使用 C 风格声明和控制流，便于 C/C++ 开发者理解。
- 字段声明只读，并以确定的顺序消耗输入。
- 每个编码语法字段都精确映射到一个或多个源区间。
- 计算字段不伪装成具有源位置的编码字段。
- 所有输入访问和执行工作都有边界检查、资源上限并且可以取消。
- 语言不能访问宿主文件、网络、进程、指针或原生库。
- 使用组合复用结构；不提供 C++ 对象生命周期、继承、模板和可变对象语义。

## 字段声明

字段声明从当前视图消耗 bit，并创建语法字段。计算字段只派生一个命名值，不消耗输入。

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;

    computed<bool> is_vcl =
        nal_unit_type >= 1 && nal_unit_type <= 5;
}
```

最终参考文档必须分别定义基本类型、有无符号、字节序、bit 顺序、溢出行为、数组、枚举、结构、条件、分支、有界循环、纯函数、作用域、名称解析和规范注解。当前接受的最小子集有意更小，暂不包含表达式、数组和控制流。

当前接受的 M3 类型切片新增了按声明顺序保存的 enum，以及 `bits` 字段的显式字节序。
enum 声明为无符号整数命名；字段通过 `@enum(Type)` 把这些名称关联到解码值。
字节序只改变数值解释，source bit 地址仍是 MSB-first，字段 source location 仍对应实际消耗的 bit。

## DSL 0.1 最小子集

首个可执行子集使用以下语法。token 之间可以有空白，以及 `//` 或 `/* ... */`
注释。标识符使用 ASCII 字母、数字和 `_`，但不能以数字开头。整数是经过检查的
无符号十进制或 `0x` 十六进制数。字符串支持 `"`、`\\`、`\n`、`\r` 和 `\t` 转义。

```text
program       := { declaration }
declaration   := { annotation } ( enum | struct | sequence | entry )
enum          := "enum" identifier "{" { enum_member } "}" [ ";" ]
enum_member   := identifier "=" integer ";"
struct        := "struct" identifier "{" { field } "}" [ ";" ]
field         := { annotation } "bits" "<" integer [ "," identifier ] ">" identifier
                 { annotation } ";"
sequence      := "sequence" "<" identifier ">" identifier "="
                 "scan" "(" identifier ")" ";"
entry         := "entry" identifier ";"
annotation    := "@" identifier [ "(" [ value { "," value } ] ")" ]
value         := integer | string | identifier
```

该子集的静态规则如下：

- 程序必须且只能有一个 `entry`；target 必须是已声明的结构或 sequence。
- 结构名和 sequence 名在整个程序中不能重复；字段名在结构内不能重复；结构至少包含一个字段。
- enum 名与结构、sequence 共用声明命名空间；enum member 名在所属 enum 内不能重复。
  不同 member 可以使用相同整数值；这些别名接受同一个解码数值。
- `bits<N>` 的宽度必须是 `1..64` 的整数。字段按声明顺序以 MSB-first 消耗输入。
  省略第二个类型参数或写成 `big` 时，得到大端无符号值。`little` 只允许宽度为 8 的倍数、
  字段在结构内从字节边界开始，并且执行时 source span 也从字节边界开始；它只反转完整字节
  的数值权重，不改变实际消耗的 bit 顺序。
- 唯一接受的渐进 sequence 形式是
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`。
  `Element` 必须是已声明结构。
- `@equals(integer)` 字段注解是会执行检查的约束，在一个字段上最多出现一次，且参数值
  必须能由该字段的无符号 bit 宽度表示；`@enum(Type)` 最多出现一次，参数必须是已声明
  enum 类型，并且它的每个 member 值都必须能由字段宽度表示。enum 字段仍解码为无符号整数，
  enum 为该数值提供名称和有效值检查。`@description("text")` 提供项目编写的
  展示说明，`@spec("standard", "clause")` 提供规范引用。字段默认继承所属结构的规范
  引用，也可以用自己的注解覆盖。
- 出现词法或静态诊断时，source 不会生成可执行规则；parser 仍返回部分 IR 以及带行列范围的全部诊断，便于编辑器一次报告多个错误。

`enum`、`big` 和 `little` 只在上述语法位置作为上下文关键字，其他位置仍可作为普通
identifier。既有 `bits<N>` source 不变，并且与 `bits<N, big>` 完全等价；本切片不弃用
任何已接受的 0.1 语法。

parser 生成面向 source、用于诊断的声明模型。静态 compiler 把 enum、结构、sequence 和
entry 引用解析成 typed program，保留声明顺序，并确定性生成 `begin-structure`、
`read-unsigned-bits`、`assert-equals` 和 `end-structure` bytecode。read 指令携带已解析的
enum 与字节序类型信息，不增加另一套 source-coordinate 操作。parser 或 compiler 出现任何
诊断时都不生成可执行 typed IR。`svtool rule check` 会运行这两个阶段；内置 Annex B runner
也只在 analyzer 创建时编译一次规则，之后按已解析的结构索引执行每条记录。

最小 VM 通过 bounded bit reader 按顺序执行结构。成功字段会成为带无符号解码值和
源位置的 syntax-field 节点。小端字段在读取完成后反转完整 source byte 的数值权重，
不会改变 bit reader position 或 source mapping；若执行到该字段时绝对 source 地址没有
按字节对齐，则 typed execution 非法，并且不消耗该字段。enum 字段保留数值和类型 metadata；
若数值不属于已声明 member，则保留字段节点，把结构标记为 invalid，并在字段位置报告
`invalid-syntax`。读取截断或失败时保留之前的字段，并把结构标记为 invalid 并附 source
诊断。`@equals` 不匹配时保留该字段，再用 invalid-syntax 诊断标记结构。最小执行器要求
逻辑范围映射到一个连续的 direct source 区间；跨多个 source 区间的 mapped transformation
留到后续转换运行时实现。执行器把字段类型、说明和规范引用保留在 analysis-node snapshot
上；展示宽度由节点的逻辑范围推导。

内建 `h264_start_code` scanner 通过 64 KiB 随机访问窗口读取 source，并按记录数和已检查
source position 数量双重限制每个 batch。默认每次最多检查 64 KiB source position，单调
递增的 scan cursor 用于 UI 进度。每条记录包含三字节或四字节 start code 的 source 区间，
以及后续 NAL unit 区间（最后一个空 unit 没有 payload 区间）。start code 可以跨窗口。
每检查至少 1,024 个 source position 就检查一次取消；已经发布的记录保持有效，batch 返回
`cancelled`。空 unit 虽然没有可选 payload 区间，其 NAL offset 和零 length 仍然有效。
如果 source 字节大小无法用 64 位源 bit 坐标模型表示，scanner 会在读取前拒绝该 source。

内置 H.264 Annex B 候选探测器最多检查 source 已加载前缀的前 64 KiB，不使用文件名或
扩展名猜测格式。每个完整的三字节或四字节 start code 都会形成带 source 位置的证据；
如果后续字节可用，证据还会记录 NAL unit header 的 source 区间、
`forbidden_zero_bit` 检查结果和 `nal_unit_type`。

只有完整 start code、但没有语法上可信 header 时，证据为 `weak`。一个满足
`forbidden_zero_bit == 0` 且 `nal_unit_type` 位于 `1..23` 的 header 会把候选置信度提升为
`probable`；两个或更多这样的 header 会提升为 `strong`。结果同时报告实际检查的字节数，
以及该前缀是否已经覆盖完整 source。若 64 KiB 只覆盖 source 的一部分，那么没有候选只
表示探测范围内未发现 Annex B signature，不能据此拒绝 source 或永久决定所用规则。
格式探测始终只是推荐，显式规则选择可以覆盖它。

### 内置 Annex B profile

内置最小 H.264 规则使用上面的语法；本节的分析树投影属于 profile/运行时行为，不是新增
DSL 语法。Annex B runner 为每条 scanner record 发布一个 `nal_unit[index]` region，其源位置
覆盖 start code 和非空 NAL payload。它的 `start_code` 子节点只覆盖三字节或四字节前缀。
`NalUnitHeader` 子节点只消耗 payload 的前 8 bit，并公开 `forbidden_zero_bit`、
`nal_ref_idc` 和 `nal_unit_type`；最小 profile 不解释 header 之后的 payload bit。

最后一个空 NAL 仍会发布 NAL region 和 start-code 子节点，并发布一个没有字段的 invalid
`NalUnitHeader`；所属 NAL 的 `truncated-source` 汇总诊断锚定在已知 NAL region。
`@equals(0)` 不匹配时保留 `forbidden_zero_bit`，把 header 和所属 NAL 标记为 invalid，
但不阻止整体扫描完成。header 读取失败时保留已发布节点，把 root 标记为 invalid，并返回
`source-error`；取消时保留已完成的 NAL region，并把 root 标记为 cancelled。

最小合法示例：

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
}

@index(progressive)
sequence<NalUnitHeader> nal_units = scan(h264_start_code);
entry nal_units;
```

enum 与显式 endian 的合法示例：

```cpp
enum PacketKind {
    payload = 1;
    control = 2;
}

struct PacketHeader {
    bits<16, little> payload_size;
    bits<8> kind @enum(PacketKind);
}

entry PacketHeader;
```

最小非法示例包括 `bits<0> flag;`、`bits<65> flag;`、`bits<12, little> value;`、
位于未对齐字段之后的小端字段、`@enum(Missing)`、无法放入字段宽度的 enum member 值、
重复 enum member 名、缺少 `@index(progressive)` 的 sequence、`scan(other_scanner)`、
重复声明同名，以及没有 `entry` 的程序。enum 和字段解析在下一个 member/field 分号或
右花括号处恢复，并保留全部 source range 和诊断。

## 源坐标与逻辑坐标

未经修改的媒体源使用绝对源坐标。逻辑视图拥有自己的逻辑坐标，同时保存经过所有父视图返回绝对源区间的有序映射。一个语法字段可以映射到多个不连续的源区间。

字节序属于数值解释规则，不属于坐标规则。显式 `little` 因此不会改变逻辑范围、绝对
source spans、selection 或诊断位置；这些坐标与默认大端读取完全相同。

选择语法字段时，高亮它映射到的全部源区间；选择原始 bit 时，定位到当前已经物化的
最具体节点，并保留它在分析树中的完整父级路径。解析顺序按
[分析模型](../analysis-model.md#source-bit-定位)定义的树深度、source 覆盖长度和稳定
节点 ID 确定。

## 保持映射的转换

映射转换可以转发、跳过或切分输入，同时保留每一个被转发 bit 的来源。被排除的源区间仍然显示，并携带命名的结构角色。

```cpp
view rbsp from ebsp {
    while (!input.eof()) {
        if (next_is_emulation_prevention_byte()) {
            skip bits<8> as emulation_prevention_byte;
        } else {
            forward bits<8>;
        }
    }
}
```

规则不能凭空生成逻辑 bit，然后把它暴露成具有源位置的语法字段。无法精确映射到源数据的值必须表示为计算字段。

## 惰性区域与渐进索引

规则必须显式声明可以延后物化内容的安全边界。渐进索引在可取消、可恢复的扫描过程中分批发布已发现结构。

```cpp
struct Mp4Box {
    be_u32 size;
    fourcc type;

    @lazy(size - 8)
    bytes payload;
}

@index(progressive)
sequence<NalUnit> nal_units = scan(h264_start_code);
```

惰性边界只有在其长度和外层限制都通过检查后才能注册。分析模型必须区分尚未物化、正在索引、已取消、不支持、非法和完全物化等状态。

## 沙箱与资源限制

规则只能以有界、只读方式访问当前媒体源。运行时限制执行步数、输入输出范围、递归深度、
节点数量、内存，以及两次取消检查之间的执行时间。规则不能访问任意文件、网络、进程、
环境变量、宿主指针或原生插件。

当前 VM 对一次结构物化采用以下默认限制：

- 最多执行 1,000,000 条 bytecode 指令；
- 调用深度 64，mapped view 深度 64；
- analysis node 深度 256，root 计为深度 1；
- 最多新建 100,000 个物化节点；
- 第一条指令执行前检查一次取消，之后至少每执行 1,024 条指令检查一次。

enum 成员检查和字节序转换都属于现有的字段读取操作，不增加 source 读取或 analysis node，
并使用同一套 instruction budget 和取消检查边界。

所有限制都必须大于零。host 可以为一次执行降低限制，但规则本身不能提高或读取限制。
当前最小子集尚无嵌套调用或 view；加入这些操作时必须消耗已经保留的深度预算。超过指令、
节点数量或节点深度限制时报告 `resource-limit`，保留超限前已完成的节点，并把当前结构标记
为 invalid。取消时报告 `cancelled`，保留已完成节点，并把当前结构标记为 cancelled；如果
取消发生在 `begin-structure` 之前，则标记其 parent。非法或损坏的 typed bytecode 会作为
invalid definition 被拒绝，运行时不会猜测执行。

输入输出、内存和 wall-clock 的精确默认值仍是暂定设计；使用这些预算的语言功能进入稳定
状态前，必须补齐对应契约。

## 规则包

规则包具有版本，并声明格式身份、引擎兼容范围、适用性元数据和依赖。格式探测只推荐候选规则，用户始终可以手动覆盖。分析会话保存最终采用的规则及其精确版本。

应用、DSL 语言和规则包分别独立版本化。规则包清单声明精确的语言契约和引擎兼容范围。在 DSL `0.x` 阶段可以进行有文档记录的破坏性修改；语言进入 `1.0` 后，不兼容修改必须使用新的语言 major 版本。引擎遇到不兼容规则包时必须拒绝加载并报告诊断，不能猜测执行。

首版规则包自包含，分析时不能通过网络解析依赖。规则包清单语法、依赖规则和信任策略尚待设计。

官方规则与特定应用版本一起发布。首版只允许从本地文件或目录安装其他规则包，不提供在线市场、自动下载或自动更新。安装前显示包身份、版本、格式覆盖、作者元数据、内容哈希和兼容范围。已保存会话锁定实际采用的精确规则版本；规则不会因为作者或“可信”声明而获得额外权限。

### 暂定规则包结构

开发时，规则包是一个目录，其中包含 TOML 清单、C 风格格式定义、本地化文档和可分发测试：

```text
org.streamview.h264/
├── rule.toml
├── src/
├── docs/
│   ├── en/
│   └── zh-CN/
└── tests/
```

本地安装时，可以将目录编码为确定性的 ZIP 容器，并使用尚待确定的专用扩展名。清单用于声明包身份、作者、许可证、规则包版本、语言契约、引擎兼容范围、入口点、格式/profile/解析深度覆盖、探测元数据和本地化文档。

发布的规则包不能包含原生可执行代码或符号链接。安装器必须拒绝绝对路径、父目录穿越、重复或未规范化路径，以及任何逃逸包根目录的条目。安装内容按内容哈希只读保存。具体清单字段、归档规范化算法、文件扩展名和大小限制仍是暂定设计。
