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

## DSL 0.1 最小子集

首个可执行子集使用以下语法。token 之间可以有空白，以及 `//` 或 `/* ... */`
注释。标识符使用 ASCII 字母、数字和 `_`，但不能以数字开头。整数是经过检查的
无符号十进制或 `0x` 十六进制数。字符串支持 `"`、`\\`、`\n`、`\r` 和 `\t` 转义。

```text
program       := { declaration }
declaration   := { annotation } ( struct | sequence | entry )
struct        := "struct" identifier "{" { field } "}" [ ";" ]
field         := { annotation } "bits" "<" integer ">" identifier
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
- `bits<N>` 的宽度必须是 `1..64` 的整数。字段是无符号值，按声明顺序以 MSB-first 消耗输入。
- 唯一接受的渐进 sequence 形式是
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`。
  `Element` 必须是已声明结构。
- `@equals(integer)` 字段注解是会执行检查的约束；其他注解保留为元数据。
  `@spec("standard", "clause")` 是约定的规范引用形式。
- 出现词法或静态诊断时，source 不会生成可执行规则；parser 仍返回部分 IR 以及带行列范围的全部诊断，便于编辑器一次报告多个错误。

最小运行时通过 bounded bit reader 按顺序执行结构。成功字段会成为带无符号解码值和
源位置的 syntax-field 节点。读取截断或失败时保留之前的字段，并把结构标记为 invalid
并附 source 诊断。`@equals` 不匹配时保留该字段，再用 invalid-syntax 诊断标记结构。
最小执行器要求逻辑范围映射到一个连续的 direct source 区间；跨多个 source 区间的
mapped transformation 留到后续转换运行时实现。

内建 `h264_start_code` scanner 通过 64 KiB 随机访问窗口读取 source，并按调用方指定
的批大小发布 `H264StartCodeRecord`。每条记录包含三字节或四字节 start code 的 source
区间，以及后续 NAL unit 区间（最后一个空 unit 没有 payload 区间）。start code 可以跨窗口。
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

最小非法示例包括 `bits<0> flag;`、`bits<65> flag;`、缺少 `@index(progressive)` 的
sequence、`scan(other_scanner)`、重复声明同名，以及没有 `entry` 的程序。

## 源坐标与逻辑坐标

未经修改的媒体源使用绝对源坐标。逻辑视图拥有自己的逻辑坐标，同时保存经过所有父视图返回绝对源区间的有序映射。一个语法字段可以映射到多个不连续的源区间。

选择语法字段时，高亮它映射到的全部源区间；选择原始 bit 时，定位到当前已经物化的最具体字段，并保留它在分析树中的完整父级路径。

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

规则只能以有界、只读方式访问当前媒体源。运行时限制执行步数、输入输出范围、递归深度、节点数量、内存，以及两次取消检查之间的执行时间。规则不能访问任意文件、网络、进程、环境变量、宿主指针或原生插件。

默认限制、配置策略和诊断尚待设计；语言进入稳定状态前，必须在中英文文档中说明。

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
