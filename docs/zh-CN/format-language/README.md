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

    assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;

    computed<bool> is_vcl =
        nal_unit_type >= 1 && nal_unit_type <= 5;
}
```

最终参考文档必须分别定义基本类型、有无符号、字节序、bit 顺序、溢出行为、数组、枚举、
结构、条件、分支、有界循环、纯函数、作用域、名称解析和规范注解。当前接受的最小子集
仍有明确边界：表达式只能出现在纯函数返回值、计算字段、lazy byte count、dynamic bit width
和 assertion condition 中；imported value 仍局限在专用 dynamic-width、equality-guard 与
source-anchored assertion-condition 形式。控制流只包含下述条件、switch 和有界 repeat 形式。

当前接受的 M3 类型切片新增了按声明顺序保存的 enum，以及 `bits` 字段的显式字节序。
enum 声明为无符号整数命名；`bits` 或 `ue` 字段通过 `@enum(Type)` 把这些名称关联到解码值。
字节序只改变数值解释，source bit 地址仍是 MSB-first，字段 source location 仍对应实际消耗的 bit。

当前接受的变长 primitive 切片新增了 H.264 风格的无符号和有符号 Exp-Golomb
字段，类型关键字为 `ue` 和 `se`。这两种类型没有显式宽度或 endian 参数；其 source
location 覆盖完整编码码字，而不是固定 bit 数。

当前接受的固定数组切片允许在 scalar 字段名后写一维正整数长度，例如
`bits<8> payload[4]`、`ue codes[3]` 或 `se deltas[3]`。compiler 把声明展开为独立完成
类型检查和执行的 `payload[0]` 到 `payload[3]`；不会新增数组值、容器节点或数组专用 opcode。

当前接受的条件切片新增了可嵌套的
`if (previous_field == integer) { ... } else { ... }` block，`else` 可以省略。
等值形式接受此前且路径上保证存在的 scalar `bits`、enum、`ue` 或 `computed<u64>` controller；
Boolean 简写接受此前且路径上保证存在的 `computed<bool>`。两种形式都不是一般条件表达式。

当前接受的 switch 切片新增了可嵌套的
`switch (previous_field) { case integer: { ... } default: { ... } }` block。
每个 arm 都有自己的花括号 body，`default` 可以省略，选择逻辑复用条件切片的受限等值
guard。

当前接受的有界 repeat 切片新增了可嵌套的
`repeat (previous_count, maximum) { ... }` block。此前解码出的无符号计数选择零到
`maximum` 次投影迭代；正整数字面量 `maximum` 同时约束编译后的字段投影与运行时可接受
计数。这不是通用 loop 或计数表达式。

当前接受的 bounded-sentinel 切片新增 post-tested
`repeat (maximum) { ... } until (field == integer);` block。body 至少执行一次，终止项本身也会
物化；`1..64` 范围内的正整数 maximum 限制全部 projected iteration。

当前接受的计算值切片新增了顶层、表达式体的 scalar `pure` 函数，以及结构内的
`computed<bool>` 或 `computed<u64>` 计算字段。pure call 会在编译期内联成有固定边界的
typed expression；计算字段不消耗 source bit，也永远没有 source location。该切片不引入
runtime call stack、可变状态或 host 访问。

当前接受的 lazy-boundary 切片新增 `@lazy(byte_count) bytes name;` region。runtime 会验证
其 mapped logical range，创建 lazy analysis node，并在不读取或复制 payload 的情况下推进
logical cursor。本切片只注册安全的未解释 boundary；typed on-demand expansion 留给后续
切片。

当前接受的 progressive-index recovery 切片保持既有 `@index(progressive)` H.264 sequence
语法，并让 cancelled index 可以在同一个 analyzer 内恢复。它保留已发布 node、scanner 与
queue state 和单调 identifier，但不定义持久 checkpoint。

当前接受的 payload-dispatch 切片新增唯一的顶层
`payload<rbsp> sequence switch (controller) { case integer: Structure; }` 声明。
它把 sequence element 解码出的 controller 值绑定到解码派生 payload view 的结构；当该
payload 不含任何语法元素时绑定到 `empty`。决定 payload 使用哪个结构的是规则，不是 runner。

当前接受的 H.264 trailing-bits 切片新增终结结构项 `rbsp_trailing_bits;`。它会消费必需的
stop bit 与依当前位置决定的 RBSP 补零，而不引入通用对齐表达式或无界 loop。

当前接受的 compressed-payload 切片新增有名称的终结结构项
`compressed_payload name;`。它把有界 logical view 中全部剩余 bit 发布为 materialized
opaque payload，并在不读取或复制这些 bit 的情况下推进到 view 末尾。

当前接受的 context-publication 切片新增结构 annotation `@context`、可重复的
`@context_dependency`，以及 scalar field annotation `@context_export`。rules-owned
execution session 将完整 definition 及选中的 typed value 发布到 position-aware directory，
不会从 presentation node 回读值。

当前接受的 context-import 切片新增可重复 structure annotation `@context_import`。consumer
structure materialize 后，同一 session 在 consumer position 选择声明的 generation，并返回
其 rules-owned export payload 与精确 dependency closure。imported value 不进入一般
expression namespace。

当前接受的 imported dynamic-width 切片允许用 checked unsigned arithmetic expression 作为
big-endian `bits` 字段的宽度。保留形式
`context_value(import_key, context_kind, exported_field)` 会在读取字段前从精确 imported
generation closure 解析一个 scalar。

当前接受的 imported-condition 切片还允许把该保留形式作为
`context_value(...) == integer` 的左侧。它仍是 `u64` leaf，不进入一般 expression 或
controller namespace。

当前接受的 source-anchored assertion 切片新增 structure statement
`assert(boolean_expression) at source_field;`。rule 可以用它强制字段间的致命关系，而不创建
presentation node；`at` 字段提供 diagnostic path 与精确 mapped source location。

当前接受的 imported-assertion 切片还允许该 Boolean condition 使用保留的
`context_value(...)` leaf。它沿用 exact imported generation 合同，在其他一般 expression
position 中仍不可用。

当前接受的 computed-initializer 切片还允许 `computed<bool>` 或 `computed<u64>` 的
initializer 内部使用这两个保留 leaf，且遵循完整 expression 语法，而不是固定的比较形状。
它让派生字段能够依赖 parameter set，或依赖自己所属的 element header；多子句的存在性
guard 与依赖性 repeat count 正是这样表达的。

当前接受的 optional-value 切片新增保留形式
`optional_value(field_identifier, fallback_expression)`。当实际执行的路径物化了被命名的
字段时它给出该字段的值，否则给出 fallback 的值；一个可能被某个分支覆盖的值，正是这样
抵达要求“每条路径都必须有值”的 position 的。它是 branch-guarantee 规则唯一的豁免之处，
而且只有第一个实参被豁免。

当前接受的 RBSP source-state 切片新增保留的零参数 leaf `more_rbsp_data()`。它在不推进
reader 的前提下，报告当前 logical remainder 是否不是完整的 H.264 trailing pattern。
它可以用于 structure 执行期表达式，但不能用于 pure function body。

当前接受的 sequence-element 切片新增保留形式 `header_value(element_field)`。它在被派发的
payload structure 内部解析 sequence element structure 的一个 scalar，使 payload 能够依赖
自己所属的 element header，而不是依赖 parameter set。它是 `u64` leaf，允许出现的位置与
imported `context_value` leaf 完全相同；并且由于它是 call 而不是 identifier，element 与
payload 的字段命名空间保持分离。

## DSL 0.1 最小子集

首个可执行子集使用以下语法。token 之间可以有空白，以及 `//` 或 `/* ... */`
注释。标识符使用 ASCII 字母、数字和 `_`，但不能以数字开头。整数是经过检查的
无符号十进制或 `0x` 十六进制数。字符串支持 `"`、`\\`、`\n`、`\r` 和 `\t` 转义。

```text
program       := { declaration }
declaration   := pure_function
               | { annotation } ( enum | struct | sequence | payload | entry )
pure_function := "pure" scalar_type identifier "("
                 [ parameter { "," parameter } ] ")"
                 "{" "return" expression ";" "}"
parameter     := scalar_type identifier
enum          := "enum" identifier "{" { enum_member } "}" [ ";" ]
enum_member   := identifier "=" integer ";"
struct        := "struct" identifier "{" { struct_item } "}" [ ";" ]
struct_item   := field | computed | lazy_region | assertion
               | rbsp_trailing_bits
               | compressed_payload
               | conditional | switch | repeat
field         := { annotation } field_type identifier [ "[" integer "]" ]
                 { annotation } ";"
field_type    := "bits" "<" additive [ "," identifier ] ">" | "ue" | "se"
computed      := { annotation } "computed" "<" scalar_type ">" identifier
                 "=" expression { annotation } ";"
lazy_region   := "@" "lazy" "(" expression ")" "bytes" identifier
                 { presentation_annotation } ";"
assertion     := "assert" "(" expression ")" "at" identifier ";"
rbsp_trailing_bits := "rbsp_trailing_bits" ";"
compressed_payload := "compressed_payload" identifier
                      { presentation_annotation } ";"
presentation_annotation := "@" "description" "(" string ")"
                         | "@" "spec" "(" string "," string ")"
scalar_type   := "bool" | "u64"
conditional   := "if" "(" ( identifier "==" integer | identifier
                               | context_value "==" integer ) ")"
                 "{" { struct_item } "}"
                 [ "else" "{" { struct_item } "}" ]
context_value := "context_value" "(" identifier "," identifier ","
                 identifier ")"
header_value  := "header_value" "(" identifier ")"
optional_value := "optional_value" "(" identifier "," expression ")"
more_rbsp_data := "more_rbsp_data" "(" ")"
switch        := "switch" "(" identifier ")" "{"
                 switch_case { switch_case } [ switch_default ] "}"
switch_case   := "case" integer ":" "{" { struct_item } "}"
switch_default := "default" ":" "{" { struct_item } "}"
repeat        := "repeat" "(" identifier "," integer ")"
                 "{" { struct_item } "}"
               | "repeat" "(" integer ")" "{" { struct_item } "}"
                 "until" "(" identifier "==" integer ")" ";"
sequence      := "sequence" "<" identifier ">" identifier "="
                 "scan" "(" identifier ")" ";"
payload       := "payload" "<" identifier ">" identifier
                 "switch" "(" identifier ")" "{" payload_case { payload_case } "}"
payload_case  := "case" integer ":" ( identifier | "empty" ) ";"
entry         := "entry" identifier ";"
annotation    := "@" identifier [ "(" [ value { "," value } ] ")" ]
value         := integer | string | identifier
expression    := logical_or
logical_or    := logical_and { "||" logical_and }
logical_and   := equality { "&&" equality }
equality      := relational { ( "==" | "!=" ) relational }
relational    := additive { ( "<" | "<=" | ">" | ">=" ) additive }
additive      := multiplicative { ( "+" | "-" ) multiplicative }
multiplicative := unary { ( "*" | "/" | "%" ) unary }
unary         := "!" unary | primary
primary       := integer | "true" | "false" | identifier
                 | identifier "(" [ expression { "," expression } ] ")"
                 | "(" expression ")"
```

该子集的静态规则如下：

- 程序必须且只能有一个 `entry`；target 必须是已声明的结构或 sequence。
- 结构、sequence、enum 和纯函数名共用顶层声明命名空间。语法字段、计算字段、lazy byte
  region 与 compressed payload 的名称在结构内统一保持唯一；结构至少包含其中一个 item，或一个终结
  `rbsp_trailing_bits` item。
- enum member 名在所属 enum 内不能重复。不同 member 可以使用相同整数值；这些别名接受
  同一个解码数值。
- 纯函数声明 `bool` 或 `u64` 返回类型、最多 16 个名称互异的 `bool` 或 `u64` 参数，以及
  唯一一条 `return` 表达式。函数只能引用自己的参数和此前声明的纯函数。函数 overload、
  forward call、直接或间接递归，以及纯函数上的 annotation 都会被拒绝。每个函数体即使
  没有被使用，也会独立完成类型检查和表达式边界检查。
- 字面量 `bits<N>` 的宽度必须是 `1..64` 的整数。非字面量宽度是 checked `u64`
  arithmetic expression，只允许用于 big-endian、非数组的 `bits` 字段；runtime 结果同样必须
  位于 `1..64`。dynamic 字段不能使用 enum 或 equality constraint，也不能作为 context
  key、dependency、import 或 export；它可以使用 semantic `@range` constraint。它的 runtime
  width 会使后续精确静态 offset 变为未知。
  字段按声明顺序以 MSB-first 消耗输入。
  省略第二个类型参数或写成 `big` 时，得到大端无符号值。`little` 只允许宽度为 8 的倍数、
  字段在结构内从字节边界开始，并且执行时字段的绝对逻辑起点与首个解析出的 source bit 都
  从字节边界开始；后续 mapping segment 的 source 边界不要求按字节对齐。它只反转完整逻辑
  字节的数值权重，不改变实际消耗的 bit 顺序。
- `ue` 和 `se` 是变长 Exp-Golomb 字段，不接受宽度或 endian 参数，并始终按
  MSB-first 消耗编码码字。由于宽度不能静态确定，变长字段之后的小端字段会被拒绝，除非
  后续语言功能能够证明其对齐。无符号 `ue` 字段可以带一个 `@equals(integer)` constraint。
  无符号固定或动态宽度 `bits` 字段、`ue` 字段与有符号 `se` 字段都可以带一个
  `@range(minimum, maximum)` constraint；有符号 `se` 字段不接受 `@equals`。
- `ff_coded<max_bytes>` 是变长字节累加标量字段编码（ADR-0079）。`max_bytes` 是
  必须的编译期正整数字面量，用于约束允许读取的最大字节数（`1 <= max_bytes <= 64`）。
  解码器按顺序连续读取 8-bit 字节，对每个 `0xFF` 字节累加 255，并在读取到首个小于
  `0xFF` 的字节时结束解码。该字段产出无符号标量 `u64` 值（`255 * N + last_byte`），
  其 source span 完整覆盖所消费的连续字节范围。若读取达到 `max_bytes` 仍未遇到终止
  字节（< 0xFF），解码失败并生成 `invalid-syntax` 诊断。
- scalar 字段可以带一个 `[count]` 固定数组后缀。`count` 必须是大于零的无符号整数字面量；
  不接受表达式、额外维度、结构数组或运行时长度。重复名称检查仍使用声明的基础字段名。
  一个结构展开后最多投影 99,999 个 scalar 字段，从而在默认 100,000 个物化节点预算内
  为结构节点保留一个名额；超过该上限时 compiler 不生成可执行 typed IR。
- `@context(kind, key_field)` 在一个结构上至多出现一次。`kind` 只能是 `h264-sps`、
  `h264-pps`、`aac-asc` 或 `iso-bmff-sample-description`。
  `@context_dependency(kind, key_field)` 只允许出现在同时具有 `@context` 的结构上，最多
  出现 16 次；相同 kind/field pair 是静态重复错误，不会合并。
- 每个 context key 与 dependency key 都必须命名同一结构中无条件、顶层、非数组的
  unsigned scalar。允许的 scalar kind 是 `bits`、enum、`ue` 和 `computed<u64>`；signed
  field、guarded/repeated field、数组元素、lazy region 与生成的 trailing-bit field 都不是
  context value。
- `@context_export` 不接受参数，在一个 field 上至多出现一次，并且只允许标注具有
  `@context` 的结构中同类无条件 unsigned scalar。一个 definition 最多 export 64 个值。
- `@context_import(kind, key_field)` 在一个 structure 上最多出现 16 次。它使用相同的已识别
  kind 与无条件 unsigned scalar key-field 规则，保留 declaration order；相同 kind/field pair
  是静态重复错误。
- `context_value(import_key, context_kind, exported_field)` 保留给 dynamic `bits` width、
  imported equality conditional 左侧、source-anchored assertion 的 Boolean condition，或
  computed field initializer 使用。
  三个参数都必须是 identifier。`import_key` 必须命名
  此前 context-eligible 的字段，并且在该
  structure 上精确标识一个 import。kind identifier 只能是 `h264_sps`、`h264_pps`、
  `aac_asc` 或 `iso_bmff_sample_description`，而且必须命名 imported root kind，或从其声明的
  dependency graph 可达的 kind。该 target kind 必须恰好有一个 publishing structure，且该
  structure 必须精确导出一个同名字段。除 dynamic `bits` width、source-anchored assertion
  condition、computed field initializer 与精确形式 `context_value(...) == integer` 外，
  pure-function body、condition、lazy size、switch controller 与其他 expression position
  都拒绝这一形式。computed initializer 按完整 expression 语法接受它，因此可以与算术和
  Boolean 运算符组合；由于 `computed<u64>` 可以充当 count repeat 的 controller，imported
  的表项数量是通过该字段而不是直接抵达 repeat controller 的。
- `header_value(element_field)` 保留给与 `context_value` 相同的四个位置使用。它接受且只接受
  一个 identifier 实参，该实参必须命名程序中 sequence element structure 的一个字段，且该字段
  必须是无条件、顶层、非数组的 unsigned scalar；被 guard 的、位于 repeat 中的、数组、
  dynamic-width 或 signed element 字段一律拒绝。没有声明 sequence 的程序，以及 structure
  本身就是 sequence element structure 的情况，都会被拒绝。该 leaf 在程序范围内针对 element
  structure 解析，因此写在从不被派发为 payload 的 structure 中的调用能够编译通过，但会在
  执行期以 invalid definition 失败。
- `optional_value(field_identifier, fallback_expression)` 保留给 dynamic `bits` width、
  source-anchored assertion condition、computed field initializer 与 lazy byte count 使用。
  pure-function body、condition、switch controller 与其他任何 expression position 都拒绝
  这一形式，imported 与 sequence-element 的等值 conditional 形式同样拒绝。它接受且只接受
  两个实参：第一个必须是 identifier，第二个是按完整语法书写的 `u64` expression。leaf 本身
  的类型是 `u64`。
- 第一个实参必须命名同一 structure 中更早声明的 scalar unsigned `bits`、enum、`ue` 或
  `computed<u64>` 字段。数组、`se`、`computed<bool>`，以及未知或未来字段与在别处一样被
  拒绝。它仅仅豁免 branch-guarantee 规则，因此声明在 conditional 内部的字段是被接受的；
  它有意不要求该字段必须是 branch-local，因为后续某个无关的 guard 不应当反过来让一处
  正确的用法失效。fallback expression 在调用处的 scope 中编译，并保留该 position 原本
  施加的每一条规则，因此 branch-local 的 fallback dependency 仍然会被拒绝。
- `power_of_two(unsigned_expression)` 是有界幂运算 leaf。`u64` exponent 为 `0..63` 时返回
  `1 << exponent`，更大的 exponent 在求值时返回致命 `invalid-syntax`。它遵守完整 expression
  grammar，不读取 source，也不新增 presentation node。
- `more_rbsp_data()` 是不接受参数、类型为 `bool` 的保留 source-state leaf。它可用于
  structure 执行期表达式，包括 computed initializer 与 assertion condition，但 pure-function
  body 会拒绝它，pure function 也不能使用该保留名。求值不推进当前 reader：剩余零 bit 时
  返回 false，多于八 bit 时返回 true；剩余一至八 bit 时，只有完整 remainder 恰好为 `1`
  后全零才返回 false。探测失败会沿用现有 truncated-source 或 source-error 状态传播，且
  cursor 不移动。
- 固定宽度数组按 `width * count` bit 参与静态对齐。小端数组的每个元素宽度必须是 8 的
  倍数，并且首元素必须从结构内的字节边界开始。`ue` 或 `se` 数组的总宽度未知，因此其
  后续小端字段与单个 Exp-Golomb 字段之后的小端字段一样会被拒绝。
- `rbsp_trailing_bits;` 是不接受 annotation 的 H.264 终结项。它只能出现一次，只能作为结构
  顶层的最后一项；不能位于 conditional、switch 或 repeat body 中，也不能后跟其他 item。使用
  它的结构会保留名称 `rbsp_stop_one_bit` 和 `rbsp_alignment_zero_bit`。运行时先读取一个值必须
  为 `1` 的 stop 字段，再读取零至七个值必须为 `0` 的 alignment 字段，直到下一个 logical-byte
  boundary。每个实际消费的 bit 都有独立命名的 syntax-field node 与 mapped source location。
  缺失 bit 为 `truncated-source`；约束失败会在对应字段上报告 `invalid-syntax`。compiler 会把
  八个可能字段都计入 99,999-field 与 100,000-node 限制；VM 使用一条 bytecode instruction，
  且只发布实际消费的 alignment 字段。
- `compressed_payload name;` 是有名称的剩余 bit 终结项。它在一个结构中至多出现一次，只能
  无条件作为顶层最后一项，并与 `rbsp_trailing_bits` 互斥。conditional、switch 与 repeat body
  中拒绝该 item；它不接受前置 annotation 或数组后缀，只接受尾随 `@description` 和
  `@spec`。其名称进入结构统一字段命名空间。runtime 会映射当前有界 reader 中全部剩余 bit，
  发布一个 materialized `CompressedPayload` node，并在不读取或复制 payload data 的情况下
  seek 到末尾。不按 byte 对齐、multi-span 和空 range 都合法。item 不产生 scalar value，
  不能作为 controller、expression dependency 或 context value。
- `assert(condition) at anchor;` 是不接受 annotation 的 structure item。它可以是无条件顶层
  item，也可以位于 bounded 或 sentinel repeat 内部，包括该 repeat 中嵌套的 conditional 或
  switch body；不在 repeat 中的 conditional/switch 仍拒绝 assertion，也不能把它写在 terminal
  item 之后。`condition` 必须为 `bool`，沿用完整的 bounded expression 与 pure-function
  合同，并且可以包含上文定义的 exact imported `context_value` leaf、
  `header_value(element_field)` leaf 与 `optional_value(...)` leaf。本地字段 dependency
  必须是此前声明、当前路径保证存在的 scalar unsigned `bits`、enum、`ue`、`computed<u64>` 或
  `computed<bool>`；array、`se`、lazy region、compressed payload、未知/未来字段与不可用的
  branch-local 值都会被拒绝。repeat-local assertion 只能引用当前静态 projection iteration
  中的值，不能 aggregate 或索引其他 iteration。
- assertion anchor 必须命名此前声明、当前路径保证存在且 source-backed 的非数组 scalar syntax
  field。fixed/dynamic `bits`、enum、`ue` 与 `se` 可以锚定 diagnostic；computed field、
  generated item 与 region item 不可以。assertion 不是字段，不引入 name 或 scalar value，不改变 static
  alignment，也不计入 99,999-field projection；它不能作为 controller 或 context value。一个
  structure 最多声明 1,024 条 assertion。
- 等值条件 controller 必须是此前声明、且在到达该条件的每条路径上都保证已经物化的
  scalar `bits`、enum、`ue` 或 `computed<u64>` 字段。数组、`se`、`computed<bool>`、
  未来或未知字段，以及离开所属保证分支后使用的 branch-local 字段都会被拒绝。整数字面量
  必须能由 source field 的无符号宽度表示；`computed<u64>` 接受完整 `u64` 字面量范围。
  简写 `if (flag)` 只接受此前且路径上保证存在的 `computed<bool>`，含义是与 `true` 相等。
  条件可以嵌套，两个分支都会执行静态检查，即使运行时只执行一个。imported equality
  只接受精确形式 `context_value(import_key, context_kind, exported_field) == integer`；value
  类型为 `u64`，literal 因此可使用完整 `u64` 范围，并遵守上面的 static import、reachability、
  unique-publisher 与 named-export 合同。当前切片不接受一般条件表达式、围绕 imported value
  的 arithmetic、Boolean combination、imported shorthand、`!=`、ordering、`else if` 或其他
  比较形式。sequence-element equality 使用对应的精确形式
  `header_value(element_field) == integer`，受同一套限制约束。
- 字段名在结构的全部分支中仍必须唯一，所有可能的 branch field 都计入 99,999 字段投影
  上限。静态对齐沿两个分支分别跟踪；只有两条路径都在相同的已知 offset 结束时，条件出口
  才保留已知 offset，省略的 `else` 按空路径处理。
- switch controller 遵守等值条件的声明顺序与路径可用性规则，接受 scalar `bits`、enum、`ue`
  或 `computed<u64>`，但不接受 `computed<bool>`。switch 至少包含一个 `case`；每个 case
  只接受一个互不重复、能由 source controller 宽度表示的无符号整数字面量；
  `computed<u64>` 接受完整 `u64` 范围。`default` 可以省略，最多出现一次，并且必须是最后
  一个 arm。当前切片不接受 fallthrough、`break`、同一 body 的多个 label、范围、enum
  member 名或一般表达式。switch 与条件可以按任意顺序嵌套。
- 每个 switch arm 都执行静态检查，字段名在全部 arm 和外层分支中仍必须唯一。所有 arm
  field 都计入 99,999 字段投影上限。每个 arm 从相同的入口静态 offset 开始；只有所有路径
  都在相同的已知 offset 结束时，switch 出口才保留已知 offset。省略 `default` 时，未匹配
  的空路径也参与合并。
- repeat controller 必须是此前声明、且在到达该语句的每条路径上都保证已经物化的无符号
  scalar `bits`、enum、`ue` 或 `computed<u64>` 字段。数组、`se`、`computed<bool>`、未知
  或未来字段，以及离开所属保证分支后使用的 branch-local 字段都会被拒绝。`maximum`
  必须是正的无符号整数字面量；使用定宽 source controller 时，它必须能被该字段表示，
  computed controller 则接受完整 `u64` 范围。不接受计数表达式、EOF 终止或
  `break`。repeat body 必须至少包含一个语法字段、计算字段或 lazy byte region，并可包含
  这些 item、条件、switch 和嵌套 repeat。
- compiler 会验证 repeat body，并将它精确投影 `maximum` 次。source 声明名在整个结构
  内仍必须唯一。body 声明可供同一次迭代内的后续 item 使用，但 repeat-local 声明在其他
  迭代或 repeat 之后不可用。物化名称按从外到内的顺序追加各层 repeat index，再追加可选
  的固定数组 index：`value[i]`、`value[outer][inner]` 和 `value[i][j]`。每个投影字段都
  计入 99,999 字段上限。
- 静态对齐会沿每个投影迭代分别检查，因此 repeat body 内的固定对齐错误会被拒绝。运行时
  可以选择零到 `maximum` 次迭代，所以 repeat 之后的结构内 offset 被视为未知；即使未来
  的表达式分析可能证明对齐，当前切片仍拒绝 repeat 之后的小端字段。
- sentinel repeat 使用 post-tested
  `repeat (maximum) { ... } until (field == integer);`，maximum 为 `1..64`。sentinel 必须直接
  声明在 body 中，并且是无条件、顶层、非数组的 fixed-width `bits`、enum 或 `ue` 字段；
  dynamic-width field、`se`、computed field、lazy region、nested declaration、body 外字段和其他
  comparison 都会被拒绝。value 必须能由 fixed width 或受支持的 `ue` domain 表示。body 至少
  执行一次并在完成后检查 sentinel，终止字段会保留；达到 maximum 仍未命中时，在最终 sentinel
  field 上产生 runtime `invalid-syntax`。命中后，后续 projection 不读取 source、不创建 node。
  local name 不逸出，每个 projection 都计入 99,999-field 上限，statement 后 static alignment 为
  unknown。不提供 `break`、`continue`、EOF 驱动的 repeat 或 expression termination；
  `more_rbsp_data()` 是 expression leaf，不是 loop controller。
- 计算字段声明 `computed<bool>` 或 `computed<u64>` 和一条表达式。它可以引用此前声明、
  且在到达当前声明的每条路径上都保证存在的 scalar 无符号 `bits`、enum、`ue` 或计算字段。
  数组、`se`、未知或未来字段，以及路径上不可用的 branch-local 值都会被拒绝。它的表达式
  还可以按完整 expression 语法包含保留的 `context_value(...)`、`header_value(...)`、
  `optional_value(...)` 与 `more_rbsp_data()` leaf，它们在别处携带的每一项约束都继续适用；若 structure 未声明
  匹配的 `@context_import`，其中的 computed initializer 会被拒绝，方式与 dynamic width
  完全一致。计算字段消耗零 bit，不改变静态对齐，继承外层 guard，计入 99,999 字段投影
  上限，并按与语法字段相同的 scope 规则供后续声明使用。repeat 投影会给它的物化名称
  追加同样的 index。
- lazy byte region 使用专用形式 `@lazy(byte_count_expression) bytes name;`。expression
  必须产生 `u64`，并沿用 computed field 对此前 scalar unsigned dependency、路径可用性、
  pure-call expansion 和 expression limit 的规则。region 不是 scalar value，不能被后续
  expression 或 controller 引用；其名称仍在整个结构中保持唯一。
- 在 struct item 的开头位置，`@lazy(` 是保留 introducer，会先于普通 annotation list 解析。
  它的参数使用完整 expression grammar，而不是普通 annotation 的 integer/string/identifier
  argument grammar；它之前不能再写普通 annotation。
- `bytes` 只接受上述专用 lazy 形式。lazy region 没有数组后缀，只接受写在 name 之后的
  `@description` 与 `@spec`。它继承外层 conditional、switch 和 repeat guard；repeat
  projection 会给物化名称追加相同 index。每个投影后的 region 都计入 99,999-item
  structure limit。
- lazy region 的 structure-relative 起点必须在静态上已知为 byte boundary。runtime-sized
  byte count 会使后续精确静态 offset 变为未知，因此既有保守 alignment rule 会拒绝之后的
  little-endian field 或 lazy byte region。byte count 使用当前 view 的 logical byte 计量。
- 表达式接受无符号整数和 Boolean 字面量、identifier、对已声明纯函数的 call、括号、一元
  `!`、checked `*`、`/`、`%`、`+`、`-`、同类型 `==` 和 `!=`、无符号大小比较，以及
  short-circuit Boolean `&&` 和 `||`，优先级如 grammar 所示。不存在隐式转换：算术和大小
  比较要求 `u64`，逻辑运算要求 `bool`，等值运算两侧类型相同，函数实参必须与形参逐一
  匹配。无符号 overflow/underflow、除零、模零和大于等于 64 的 `power_of_two` exponent 会在
  计算字段、lazy region、dynamic field 或
  assertion anchor path 上产生 runtime `invalid-syntax`。dynamic bit width 使用同一套 checked
  arithmetic；`context_value` 是它唯一额外的 leaf form，也可以作为 imported equality
  conditional 的精确左侧、用于 source-anchored assertion condition，或出现在 computed field
  initializer 之中，并计入相同 node 与
  depth 上限；完整的 width 或 assertion expression 仍受共享 expansion-work 上限约束。enum
  字段提供解码后的 `u64`；enum member 名不是本切片的 expression value。
- 每个写出的纯函数体、计算字段 expression、lazy byte-count expression、dynamic width 或
  assertion condition，以及对应的完全内联 expression，深度最多 64，节点最多 256。展开一个函数体或 expression 时，call、
  argument 与参数替换共享最多 4,096
  个 work step；即使 callee
  没有使用某个参数，该参数仍计入上限。pure call 在编译期展开，不引入 runtime call、递归、
  source/host 访问、时间、随机数或可变状态。
- 唯一接受的渐进 sequence 形式是
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`。
  `Element` 必须是已声明结构。
- 一个程序至多声明一个 payload 派发。其 view kind 必须是 `rbsp`，且必须命名一个已声明、
  并且存在对应 `entry` 的渐进 sequence。controller 必须命名该 sequence 的 element structure
  中、在顶层无条件声明的无符号 scalar `bits` 字段，宽度至多 64 bit，从而在每条路径上都
  保证存在。Exp-Golomb 字段、计算字段、数组元素、lazy region，以及位于 conditional、
  switch 或 repeat body 内部的字段一律拒绝作为 controller。case 值必须互异，并且能由
  controller 的声明宽度表示。每个 case 目标要么是已声明结构（且不得是 element structure
  本身），要么是 `empty`。不存在 `default` arm；未列出的 controller 值保持未解释 payload
  行为。
- `@equals(integer)` 字段注解是会执行检查的约束，在一个 `bits` 或 `ue` 字段上最多出现一次。
  `bits` 字段的参数值必须能由其无符号 bit 宽度表示；`ue` 接受完整的受支持无符号范围
  `0..2^64 - 2`。
- `@range(minimum, maximum)` 是可用于无符号 `bits`、`ue` 或有符号 `se` 字段的语义范围
  约束，最多出现一次。用于无符号字段时，两个参数都是无符号整数字面量，且必须满足
  `minimum <= maximum`，不接受前导 `-`。固定
  `bits<N>` 的 maximum 必须能由 `N` bit 表示；动态 `bits` 接受完整的
  `0..2^64 - 1` annotation 值域；`ue` maximum 为 `2^64 - 2`。用于 `se` 字段时，两个参数
  都可以带前导 `-`，按有符号数比较大小，并且都必须落在该编码对称的
  `-(2^63 - 1)..2^63 - 1` 值域内；`-0` 即零。它与 `@equals` 表达同一
  字段的不同约束层级，因此同一字段可以同时带 `@equals` 和 `@range`。与其他所有受检约束
  不同，`@range` 违规不属于结构性失败：它记录 H.264 clause 7.4.3 这类值域规则，而这类
  违规不会破坏后续字段的 bit 位置。详见
  [ADR-0075](../adr/0075-extend-non-fatal-ranges-to-unsigned-bit-fields.md) 与
  [ADR-0076](../adr/0076-extend-non-fatal-ranges-to-signed-fields.md)。
  `@enum(Type)` 只能出现在 `bits` 或 `ue` 字段上，最多出现一次，参数必须是已声明 enum 类型。
  `bits` enum 的每个 member 值必须能由字段宽度表示；`ue` enum member 必须位于
  `0..2^64 - 2`。enum 字段仍解码为无符号整数，enum 为该数值提供名称与致命有效值检查。
  固定 `bits` 或 `ue` 字段可以同时带 `@enum`、`@equals` 与 `@range`：先执行致命
  membership 与 equality 检查，再执行非致命 range bound。动态 `bits` 在这三种注解中只
  接受 `@range`；`se` 只接受 `@range`，拒绝 `@equals` 与 `@enum`。`@description("text")` 提供项目编写的
  展示说明，`@spec("standard", "clause")` 提供规范引用。字段默认继承所属结构的规范
  引用，也可以用自己的注解覆盖。数组声明解析出的类型、注解、metadata 和约束会分别
  应用到每个展开元素。计算字段可在声明前或表达式后使用 `@description` 和 `@spec`，但
  拒绝 `@equals`、`@enum` 和数组后缀。lazy byte region 只在 name 之后接受这两个展示
  annotation。
- 出现词法或静态诊断时，source 不会生成可执行规则；parser 仍返回部分 IR 以及带行列范围的全部诊断，便于编辑器一次报告多个错误。

`enum`、`big`、`little`、`ue`、`se`、`pure`、`return`、`bool`、`u64`、
`computed`、`lazy`、`bytes`、`true`、`false`、`if`、`else`、`switch`、`case`、
`default`、`repeat`、`until`、`assert`、`at`、`payload`、`empty`、
`rbsp_trailing_bits` 和 `compressed_payload` 只在上述语法位置作为上下文关键字，其他位置仍可
作为普通 identifier。既有 scalar 声明保持不变，`bits<N>` 仍与
`bits<N, big>` 完全等价；本切片不弃用任何已接受的 0.1 语法。

parser 生成面向 source、用于诊断的声明模型。静态 compiler 把纯函数、enum、结构、
sequence 和 entry 引用解析成 typed program，保留声明顺序，并确定性生成
`begin-structure`、`read-unsigned-bits`、`read-unsigned-exp-golomb`、
`read-signed-exp-golomb`、`evaluate-computed`、`register-lazy-bytes`、
`register-compressed-payload`、
`read-rbsp-trailing-bits`、`assert-equals`、`assert-range-minimum`、
`assert-range-maximum`、`assert-repeat-count`、`assert-sentinel-terminated`、
`assert-expression` 和 `end-structure` bytecode。每个 field opcode
必须与字段类型匹配。Exp-Golomb typed field 的静态 bit width 为零并使用默认 bit order；
unsigned Exp-Golomb 字段也可以携带已解析 enum reference，而 signed 字段不可以。无符号字段
保留可选 equality 与 range constraint；有符号字段不带这两类
constraint。固定数组按 source 顺序展开成名为 `name[0]` 到 `name[count - 1]` 的 typed field；
每个元素各自产生 read instruction，并在存在 constraint 时各自产生对应 assertion
instruction。条件 block 被降低到同一条按声明顺序排列的字段流，每个
可能字段携带引用此前 typed-field index 的已解析 presence guard；不会新增 jump opcode 或
一般控制流 bytecode。switch case 字段携带一个正向等值 guard；default 字段携带全部 case
guard 的否定合取，省略 default 时不会为未匹配路径生成字段。嵌套 switch 与条件的 guard
按外层到内层追加。
context publication 与 import 沿用同一模型：compiler 在 typed IR 中记录每个 structure 的
context definition、有序 dependency、exported-value field index，以及有序 import kind/key
index，不生成 context 专用 opcode。VM 在读取任何 source 前校验全部 context index、scalar
type、重复项与数量，即使 typed program 是直接构造而不是由 compiler 生成也一样。所有被选
字段成功执行后，VM 只向 execution session 返回声明的 publication value、import key 及其
精确 location。校验与收集沿用既有 instruction、expression、node 和 cancellation budget；
context metadata 不能产生不计预算的执行路径。

`rbsp_trailing_bits` 降低为一条 `read-rbsp-trailing-bits` instruction 与八个生成的 typed-field
slot：一个 `rbsp_stop_one_bit` 和七个可能的 `rbsp_alignment_zero_bit[index]`。instruction 会在
读取前校验这些生成字段的名称、类型、约束以及 H.264 7.3.2.11 metadata。它只发布到下一个
logical-byte boundary 所需的 stop bit 和 padding，因此未用 slot 不创建 node，也不读取 source。
该 instruction 是一个计入预算的 cancellation point；其八次独立的一 bit read 与 node 数量
仍有明确上界。

`compressed_payload` 降低为一个 typed field 和一条 `register-compressed-payload`
instruction。VM 会在 source access 前验证它是最后的 field/opcode、只带 presentation
metadata，且没有 scalar-only property。选中的 instruction 消耗一条 instruction、一个 node
slot 与一个 cancellation point；它映射 reader 的完整剩余 range，追加 materialized compressed
payload node，并在不发出 source read 的情况下 seek 到 exclusive end。

repeat body 按 `maximum` 次投影到同一条 typed-field 流。第 `i` 次迭代中的每个字段在外层
guard 之后追加一个正向 `count > i` guard；物化名称先追加当前各层 repeat index，再追加
固定数组 index。每个投影后的 repeat 语句记录 controller、maximum、首个 body 字段、外层
guard 和 source range，并在语句位置、首个 body read 之前生成一条带 guard 的
`assert-repeat-count`。这种 lowering 只新增 greater-than presence 比较和边界断言，不新增
jump、回边、可变 index 或替代 source-coordinate 操作。无符号 Exp-Golomb 值可以作为
repeat、等值条件或 switch controller。parser 或 compiler 出现任何诊断时都不生成可执行
typed IR。`svtool rule check` 会运行这两个阶段；
内置 Annex B runner 也只在 analyzer 创建时编译一次规则，之后按已解析的结构索引执行每条
记录。

sentinel repeat 会投影到同一条线性 stream。iteration zero 继承 enclosing guard；后续每个
iteration 还要求此前全部 projected sentinel 不等于 terminating value。typed IR 记录每轮起点、
sentinel field、assertion position、termination value、enclosing guard 和 source range；全部 body
projection 后发射一条 `assert-sentinel-terminated` instruction。VM 在 source read 前验证 descriptor
和每个 projection 的 guard prefix，malformed typed IR 不能让终止后的字段继续执行。

assertion 会降低成一条 declaration-order `DslTypedAssertion` descriptor，包含 typed Boolean
condition、source-backed anchor field index、statement-position field index 与 source range；它不
加入 typed field stream。对应的一条 `assert-expression` instruction 以 descriptor index 为
operand、immediate 固定为零，并在该 statement position 执行。同一位置的 assertion 保留 source
顺序；positioned operation 重合时，bytecode 固定先完成 sentinel assertion，再执行 expression
assertion、repeat-count assertion，最后读取下一字段。包含 expression assertion 的 structure 在
source access 前会完整验证三类 descriptor stream、operand、immediate、position 与顺序。

compiler 会独立 type-check 每个纯函数，再把每个 pure call 展开到使用它的计算字段中。
得到的 typed expression 只包含字面量、此前 typed-field index，以及一元或二元 operator；
不存在 call opcode。计算字段加入同一条 typed-field 流，保留声明类型和 metadata，并继承
已解析的外层 guard。它生成一条 `evaluate-computed` instruction，不推进静态 source offset，
并与语法字段一样获得 repeat index 物化名和 repeat-local scope。任何写出的表达式或展开后
的计算字段表达式超过固定节点、深度或 4,096-step expansion-work 上限时，都不会生成可执行
typed IR。

dynamic `bits` 字段继续使用既有 `read-unsigned-bits` instruction，以 typed width expression
取代字面量 width。imported leaf 会 lower 为 context-import ordinal、target definition kind、
唯一 publishing structure index 与有序 export index；runtime 不比较字段名。compiler 会证明
target kind 就是 import root，或可从声明的 context dependency 到达，并把其后每个精确静态
offset 设为未知。

imported equality conditional 会把同一个 canonical imported leaf lower 为 field-presence guard，
并带 expected `u64` literal 与 positive/negated sense。两个 branch 仍是 linear field
projection；不新增 jump 或 conditional opcode。guard identity 包含完整 import、kind、publisher
与 export descriptor，因此引用不同 imported field 的 nested condition 不会混同。

lazy declaration 以 `LazyBytes` kind 加入同一 typed-field stream，携带必需且已经内联的
`u64` byte-count expression、解析后的 presentation metadata，以及相同的外层 guard 与
repeat index。它生成一条 `register-lazy-bytes` instruction；不会加入 scalar value namespace，
其 dynamic width 会使后续精确静态 offset 变为未知。

最小 VM 通过 bounded bit reader 按顺序执行结构。执行 bytecode 前，它会验证 reader 的完整
normalized backing 是否精确等于从给定 logical start 开始的 execution mapping slice。backing
span 不匹配、乱序、缺失、多出或越界时，typed execution 非法，并且在创建 structure node 或
读取 source data 前被拒绝。成功字段会成为带解码值和逻辑范围的 syntax-field 节点；source
location 包含 execution mapping 解析出的全部 forwarded source spans，因此可以跨 source gap，
但不会包含 gap 本身。小端字段在读取完成后反转完整逻辑字节的数值权重，不会改变 bit reader
position 或 source mapping；字段的绝对逻辑起点或首个解析出的 source bit 未按字节对齐时，
typed execution 非法，并且不消耗该字段。后续 source-span 边界不要求按字节对齐。enum 字段
保留数值和类型 metadata；
若数值不属于已声明 member，则保留字段节点，把结构标记为 invalid，并在字段位置报告
`invalid-syntax`。读取截断或失败时保留之前的字段，并把结构标记为 invalid 并附 source
诊断。`@equals` 不匹配时保留该字段，再用 invalid-syntax 诊断标记结构。`@range` 违规不同：
它保留字段和已解码值，把结构留在 materialized 状态，继续解码后续字段，并在该字段节点上
附加一条带精确 source location 的 warning-severity `invalid-syntax` 诊断。一个结构可以因此
累积多条 `@range` warning。VM 会在读取 source 前验证每个 range descriptor 对应的两条相邻
bytecode instruction、operand、immediate 与顺序，并要求 descriptor 的符号性与字段编码一致：
无符号字段只能携带无符号 bound，`se` 字段只能携带有符号 bound。有符号 bound 以补码形式
存入 immediate 并按有符号数比较。固定、动态或 Exp-Golomb 字段的完整 range
已经成功消耗，因此 range 违规不会改变任何后续字段位置。同一字段上的 `@equals` 会先执行；
`@equals` 失败时该字段不再
计算 `@range`。每个展开的数组元素
都是独立 syntax-field 节点，source location 只覆盖该元素；失败时保留之前完成的元素，不为
未完成元素创建节点，并把 `Header.values[2]` 这样的展开路径写入诊断。mapped backing 的后续
span 读取失败时，reader 保持在字段起点，也不创建半成品字段节点；诊断通过同一 mapping
解析已有逻辑范围，只包含 forwarded source spans。执行器把字段类型、说明和规范引用保留在
analysis-node snapshot 上；展示宽度由节点的逻辑范围推导。

读取选中的 dynamic 字段之前，VM 会计算已经预验证的 width expression。
`RuleExecutionSession` 在 consumer enclosing source span 起点之前选择 root generation，为本次
run 缓存该精确 rules-owned dependency closure，再按 lower 后的 structure/export ordinal 选择
imported leaf。missing、future 或 stale generation 都是无 fallback 的 `dependency-unavailable`，
并在 import-key 字段处把 partial structure 标为 `waiting-dependency`。target 缺失或歧义、
payload/schema mismatch 或 malformed descriptor 都属于 invalid definition。checked arithmetic
失败或 width 不在 `1..64` 时为 `invalid-syntax`，dynamic 字段不消费 bit；读取截断会回滚到字段
起点，但诊断仍保留可用的 mapped prefix。

VM 还会在任何 source read 前把每个 imported conditional guard 验证为 canonical、无 operand
的 `u64` leaf，并验证完整 exact import/publisher/export descriptor。到达 guarded field 时，
同一个 session resolver 从 per-run closure 提供 scalar。guard 匹配时选择 field；不匹配时不
读取 source、不创建 node，也不执行 field constraint。resolver failure 沿用 dynamic-width
status，并在 import-key field 上给出 source location。即使 branch 未选中，malformed typed guard
metadata 也不能隐藏。

VM 在每次字段读取前按外层到内层的顺序验证并计算 presence guard。guard 为 false 时跳过
该字段，不消耗 source bit、不创建 analysis node，也不执行 enum member、`@equals` 或
`@range` 数值检查。选中字段沿用既有数值、诊断和 source-location 行为，因此后续选中字段紧接在上一个
选中字段结束处开始。guard metadata 引用未知、当前、未来、类型错误或当前路径不能保证存在
的 controller 时，typed definition 非法。字段定义即使位于未选分支也仍会验证，malformed
typed IR 不能藏在 false guard 后面。
因此 switch 只物化唯一匹配的 case；没有 case 匹配时，如果存在 default 则物化 default，
否则不消耗任何 arm 输入。

当 repeat 语句的外层 guard 为 true 时，边界断言会在消费任何 body 输入之前检查已保存的
controller 值。计数超过 `maximum` 时绝不截断：保留计数字段，把结构标记为 invalid，并在
该字段位置报告 `invalid-syntax`。source-backed controller 保留精确 source location；
computed controller 只报告 field path，不带 location。外层 guard 为 false 时，边界断言和
全部 body 字段都会跳过，也不要求 controller 值存在。第 `i` 次投影迭代仅在 `count > i`
时存在。只有存在的迭代才消耗 source bit 或物化节点名额；缺席迭代不创建节点或 source
location，也不执行 enum member、`@equals` 或 `@range` 数值检查。选中字段沿用既有数值、metadata、
部分结果和 source 坐标行为，诊断路径包括 `Header.value[1][0]` 这样的投影名称。非法
repeat metadata、controller guard 或 assertion 位置属于 invalid typed definition，运行时
不会猜测执行。

到达 `assert-expression` instruction 时，VM 会在不读取 source、不移动 reader、也不创建 node
的情况下计算已经预验证的 Boolean condition。结果为 `true` 时继续下一条 instruction；结果为
`false` 时立即以 `Assertion condition is false` 消息返回致命 `invalid-syntax`，保留此前全部
materialized field，并且不执行后续 field。diagnostic path 与 location 来自 `at` field 的完整
materialized range，因此跨 mapping gap 的 anchor 可以保留多个互相分离的 forwarded source
span。checked arithmetic failure 保留既有 `invalid-syntax` 消息，并使用同一 anchor。该
instruction 仍计入 instruction budget 且是 cancellation boundary；在这里触发 limit 或
cancellation 时不会计算 condition，并保留相同的 completed prefix。

执行任何 bytecode 前，VM 会验证所有 computed 或 lazy typed expression 的 metadata，
包括节点/深度上限、结果与 operand 类型、previous-field index、dependency availability
和 controller guard。runtime range、mapping 与 node-budget check 仍在选中的 field
instruction 处执行。
`evaluate-computed` instruction 的 presence guard 为 false 时，VM 跳过表达式求值和节点
创建；该 instruction 仍计入 instruction budget，并保留取消检查点。求值成功不消耗 source
bit，只创建一个带 `bool` 或无符号 64 位值、metadata 且没有 `FieldLocation` 的
`AnalysisNodeKind::ComputedField`。runtime 算术失败不会创建计算节点，会保留此前节点、把
结构标记为 invalid，并在计算字段 path 上报告不带 source location 的 `invalid-syntax`。
malformed expression 或 controller metadata 属于 invalid typed definition。子表达式工作都
计入这一条 instruction，不新增取消检查点。

选中的 `register-lazy-bytes` instruction 会计算 typed `u64` byte count，拒绝 checked
arithmetic 或 byte-to-bit overflow，并在创建 node 前验证完整 logical range 位于 enclosing
reader 内。该 range 通过 execution mapping 解析；跨 excluded source byte 时，它只保留
互相分离的 forwarded source span。正长度 range 创建 state 为 `lazy` 的 `Region`；零长度
range 创建空的 materialized `Region`。只有 location 与 node budget 都通过检查后，VM 才会 seek
越过该 range；整个过程不读取 payload。guard 为 false 时跳过 expression 求值、node 创建和
cursor 移动。

expression 或 byte-to-bit overflow 报告 `invalid-syntax`；声明范围大于 reader 剩余
enclosing range 时报告 `truncated-source`，并在 diagnostic 中保留可用 mapped prefix。
execution 未对齐、expression metadata 缺失或类型错误、非法 reference、mapping failure，
或 opcode/type 不匹配都属于 invalid typed definition。cancellation 与 resource-limit failure
发生在 node 创建或 cursor 移动之前，并保留此前完成的 field。

对 Exp-Golomb 码字，令 `leadingZeroBits` 为 marker bit 前的连续零 bit 数，`suffix` 为
随后同宽度的无符号值。`ue` 返回无符号 64 位值
`codeNum = (2^leadingZeroBits - 1) + suffix`。`se` 在 code number 为零时返回 `0`，奇数
返回 `+(codeNum / 2 + 1)`，偶数返回 `-(codeNum / 2)`，并发布有符号 64 位值；因此开头的
有符号值是 `0, +1, -1, +2, -2`。

最多允许 63 个前导零；最长合法码字为 127 bit，最大的 `ue` 值为 `2^64 - 2`。遇到第 64
个前导零时报告 `invalid-syntax`。前缀、marker 和 suffix 作为一次事务式字段读取：截断、
source failure 或溢出都会把 reader seek 回字段起点，不创建半成品字段节点，保留此前完整
字段，并在失败字段上附带 field-path 诊断。成功节点的 logical range 和 source span 覆盖完整
码字。

每个 `ue` 或 `se` 字段只占一条 VM instruction，即使该指令内部最多读取 127 bit。内部读取
不会创建额外节点，也不会新增取消点；取消仍在文档规定的 instruction 间隔检查。127-bit
上限保证单条指令的工作有界。

内建 `h264_start_code` scanner 通过 64 KiB 随机访问窗口读取 source，并按记录数和已检查
source position 数量双重限制每个 batch。默认每次最多检查 64 KiB source position，单调
递增的 scan cursor 用于 UI 进度。每条记录包含三字节或四字节 start code 的 source 区间，
以及后续 NAL unit 区间（最后一个空 unit 没有 payload 区间）。NAL unit 区间不包含下一个
start code 或 source 结尾之前最长的一段 `trailing_zero_8bits`；scanner 会把这段 framing
作为独立 source 区间公开。`00 00 00 01` 仍作为一个四字节 start code；在两个 start code
之间，更早的额外零字节属于前一个 unit 的 trailing framing。start code 可以跨窗口。
每检查至少 1,024 个 source position 就检查一次取消；已经发布的记录保持有效，batch 返回
`cancelled`。空 unit 虽然没有可选 payload 区间，其 NAL offset 和零 length 仍然有效。
如果 source 字节大小无法用 64 位源 bit 坐标模型表示，scanner 会在读取前拒绝该 source。

cancelled batch 之后，scanner owner 可以替换 cancellation token。该操作不会重置 cursor、
pending start code、trailing-zero run、inspected count 或 read window，因此后续 batch 会继续
同一次 scan，不会重放 record 或重新扫描已经完成的前缀。

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
覆盖 start code、非空 NAL payload 和单独识别出的 trailing-zero framing。它的 `start_code`
子节点只覆盖三字节或四字节前缀。`NalUnitHeader` 子节点只消耗 payload 的前 8 bit，并公开
`forbidden_zero_bit`、`nal_ref_idc` 和 `nal_unit_type`。

完整 8 bit header 可用后，official rule 会检查 clause 7.4.1 prerequisite
`nal_unit_type != 5 || nal_ref_idc != 0`。reference priority 为零的 type-5 header 会保留三个
字段，在精确的两 bit `nal_ref_idc` span 上返回致命 `invalid-syntax`，并且不映射或物化当前
NAL 的 `rbsp_payload`；scanner 继续处理下一 NAL。type-1 header 接受任意 reference
priority；slice structure 通过 `header_value(nal_ref_idc)` 读取该值，以决定
`dec_ref_pic_marking()` 是否存在。

direct header 成功后，普通非空 payload 会在不复制字节的情况下从 EBSP 映射为 RBSP logical
view。每个完整的 `00 00 03` 都会排除其中的 `03`，并把它呈现为
`emulation_prevention_three_byte[index]`；相邻的 forwarded byte 会合并为 source span。NAL
子节点固定按以下顺序出现：`start_code`、`NalUnitHeader`、可选的 `rbsp_payload`、按 source
顺序排列的零个或多个 `emulation_prevention_three_byte[index]` region，以及可选的
`trailing_zero_8bits`。NAL unit type `14`、`20`、`21` 需要当前 profile 尚未解析的 extension
header，因此 direct header 之后的字节仍保持 uninterpreted，不会传给 mapper，也无法派发。

内置规则为 `nal_unit_type` 值 `1`、`5`、`7`、`8`、`9`、`10`、`11` 声明了 payload 派发。被派发的 type 一定会经过
映射，payload 为空时也不例外，因此每个被派发的 NAL 都存在 `rbsp_payload`。type `9` 把
`AccessUnitDelimiterRbsp` 解码为 `rbsp_payload` 的子节点，公开 `primary_pic_type`、
`rbsp_stop_one_bit` 以及 `rbsp_alignment_zero_bit[0]` 到 `rbsp_alignment_zero_bit[3]`。
这些字段由它的终结 `rbsp_trailing_bits;` item 生成。type `10` 与 `11` 声明为 `empty`，要求 RBSP 长度为零。因此一字节的 access unit delimiter
可以完整解码；只有 header 的 access unit delimiter 是 `truncated-source`；只有 header 的
end of sequence 或 end of stream 是物化；而这两种 type 一旦携带 RBSP 字节即为
`invalid-syntax`。type `7` 解码 8-bit Baseline/Main/Extended SPS 核心，以及受限的 High 子集：
4:2:0 chroma、eight-bit luma/chroma、关闭 transform bypass、无 scaling matrix，并覆盖三个
已声明的 picture-order-count 语法分支。type 0 读取
`log2_max_pic_order_cnt_lsb_minus4`；type 1 读取
`delta_pic_order_always_zero_flag`、`offset_for_non_ref_pic`、
`offset_for_top_to_bottom_field`、`num_ref_frames_in_pic_order_cnt_cycle`，以及最多 255 个
signed `offset_for_ref_frame[index]`；type 2 不读取额外 SPS POC 字段。reserved POC type
在完整 Exp-Golomb 码字处致命失败。type-1 cycle count 带非致命 `@range(0, 255)` warning，
同时控制 `repeat(..., 255)`，因此超限 count 会在任何未声明 cycle entry 改变后续布局前停止。
当 `vui_parameters_present_flag` 为一时，规则还会解码有界的
Annex E.1.1 VUI core：aspect-ratio information（包括 Extended SAR）、overscan information、
video-signal 及可选 colour-description 字段、chroma sample location、timing information、
NAL/VCL HRD presence flag 与完整的有界 Annex E.1.2 HRD schedule、
`low_delay_hrd_flag`、`pic_struct_present_flag` 和完整 bitstream-restriction 分支。SPS 的
`rbsp_trailing_bits;` 仍是可选 VUI 之后的精确终结项。

两个无条件 computed node 会为 context consumer 归一化条件 SPS 字段。
`effective_log2_max_pic_order_cnt_lsb_minus4` 在 type-0 字段缺席时取零，
`effective_delta_pic_order_always_zero_flag` 在 type-1 flag 缺席时取一。两个 node 都没有
source location，并随 SPS generation 导出；slice guard 保证每个值只在匹配的 POC 分支中使用。

每个存在的 HRD flag 选择一套具有独立前缀的 schedule，包含一至 32 个 CPB entry 和四个
delay-length 字段。任一 presence flag 为一时，两个 schedule 之后都会读取共同的
`low_delay_hrd_flag`。每个 `cpb_cnt_minus1` 带非致命 `@range(0, 31)` warning，其派生 count
则控制 `repeat(..., 32)`。因此更大的 count 会保留带 source 的 warning，再在进入 schedule
entry 前由 layout-critical repeat 边界停止 SPS；scale 字段和派生 count 仍作为已解码前缀保留。

两个 chroma sample-location type 带有非致命 `@range(0, 5)`；
`max_bytes_per_pic_denom`、`max_bits_per_mb_denom` 和两个 `log2_max_mv_length_*` 字段带有
非致命 `@range(0, 16)`。SPS 的 `log2_max_frame_num_minus4` 与条件性存在的
`log2_max_pic_order_cnt_lsb_minus4` 带 clause 7.4.2.1.1 的 `@range(0, 12)`；type-1
`num_ref_frames_in_pic_order_cnt_cycle` 除了 layout-critical repeat 边界外还带
`@range(0, 255)`。违反这些非 layout bound 时会保留该字段、完整 SPS/NAL 及其后已声明字段，
只在该字段上报告带 source 位置的 `invalid-syntax` warning。其他 syntax 值仍带 source；
materialized 只表示精确消费了被选中的已声明分支，不表示完整 Annex E 语义 conformant。
稳定 DSL 缺少所需 fixed-width 或 relational constraint 时，reserved fixed-width table 值、
非零 SAR/timing 值、timing ratio、依赖 level 的 HRD bitrate/CPB/delay 关系，以及
`max_num_reorder_frames`、`max_dec_frame_buffering` 与 SPS-derived decoder limit 的关系仍不
检查。SPS/VUI/HRD core 精确消费完整 RBSP 后会发布 SPS context generation，但不表示已经
解码后续 SEI timing consumer。

已声明 VUI 字段具有以下有界含义：

| 字段 | 本切片中的含义 |
| --- | --- |
| `vui_parameters_present_flag` | 在 SPS trailing bits 前选择可选的有界 VUI core。 |
| `aspect_ratio_info_present_flag` | 表示存在 `aspect_ratio_idc` 及其可选 Extended SAR 尺寸。 |
| `aspect_ratio_idc` | 标识 sample aspect ratio；值 255 选择 Extended SAR，其他 table value 的有效性留待后续。 |
| `sar_width` | 给出 16-bit Extended SAR 水平尺寸；非零要求留待后续。 |
| `sar_height` | 给出 16-bit Extended SAR 垂直尺寸；非零要求留待后续。 |
| `overscan_info_present_flag` | 表示存在 `overscan_appropriate_flag`。 |
| `overscan_appropriate_flag` | 表示 cropped display 是否可能合适。 |
| `video_signal_type_present_flag` | 表示存在 video format、range 和可选 colour-description 字段。 |
| `video_format` | 标识 source video format；reserved table-value 检查留待后续。 |
| `video_full_range_flag` | 选择 full-range 而非 studio-range sample value。 |
| `colour_description_present_flag` | 表示存在三个 colour-description identifier。 |
| `colour_primaries` | 标识 source colour primaries；reserved table-value 检查留待后续。 |
| `transfer_characteristics` | 标识 transfer characteristics；reserved table-value 检查留待后续。 |
| `matrix_coefficients` | 标识 matrix coefficients；reserved table-value 检查留待后续。 |
| `chroma_loc_info_present_flag` | 表示存在 top-field 与 bottom-field chroma sample location。 |
| `chroma_sample_loc_type_top_field` | 标识 top-field chroma location，超出 `0..5` 时告警。 |
| `chroma_sample_loc_type_bottom_field` | 标识 bottom-field chroma location，超出 `0..5` 时告警。 |
| `timing_info_present_flag` | 表示存在 timing scale、tick count 和 fixed-rate indication。 |
| `num_units_in_tick` | 给出 32-bit clock-tick numerator；非零与 ratio 检查留待后续。 |
| `time_scale` | 给出 32-bit time scale；非零与 ratio 检查留待后续。 |
| `fixed_frame_rate_flag` | 表示 coded picture 之间的 temporal distance 是否受约束。 |
| `nal_hrd_parameters_present_flag` | 表示存在完整的有界 NAL HRD schedule。 |
| `vcl_hrd_parameters_present_flag` | 表示存在完整的有界 VCL HRD schedule。 |
| `hrd_parameters_present` | 用于选择 `low_delay_hrd_flag` 的派生 Boolean；没有 source location。 |
| `low_delay_hrd_flag` | 任一 HRD schedule 存在时表示 low-delay HRD mode。 |
| `pic_struct_present_flag` | 为后续 timing consumer 表示 picture-structure information。 |
| `bitstream_restriction_flag` | 表示存在完整的有界 bitstream-restriction 分支。 |
| `motion_vectors_over_pic_boundaries_flag` | 表示 motion vector 是否可以越过 picture boundary。 |
| `max_bytes_per_pic_denom` | 约束最大 coded-picture byte count，超出 `0..16` 时告警。 |
| `max_bits_per_mb_denom` | 约束最大 macroblock bit count，超出 `0..16` 时告警。 |
| `log2_max_mv_length_horizontal` | 给出水平 motion-vector length bound，超出 `0..16` 时告警。 |
| `log2_max_mv_length_vertical` | 给出垂直 motion-vector length bound，超出 `0..16` 时告警。 |
| `max_num_reorder_frames` | 给出可先于一个 output frame 的最大 frame 数；relational 检查留待后续。 |
| `max_dec_frame_buffering` | 给出 decoder frame-buffer bound；SPS-derived 与 relational 检查留待后续。 |

下表中的每个 `*` 表示一套独立存在的 HRD schedule 所使用的 `nal_hrd` 或 `vcl_hrd` 前缀：

| 字段 | 本切片中的含义 |
| --- | --- |
| `*_cpb_cnt_minus1` | 给出 CPB schedule 数减一，超出 `0..31` 时告警，并受 repeat contract 限制。 |
| `*_bit_rate_scale` | 给出用于派生 schedule bitrate 的四 bit exponent。 |
| `*_cpb_size_scale` | 给出用于派生 schedule CPB size 的四 bit exponent。 |
| `*_cpb_count` | 派生的 `cpb_cnt_minus1 + 1` repeat count；没有 source location。 |
| `*_bit_rate_value_minus1[i]` | 给出 indexed CPB schedule 在 scale 前的 bitrate 值。 |
| `*_cpb_size_value_minus1[i]` | 给出 indexed CPB schedule 在 scale 前的 size 值。 |
| `*_cbr_flag[i]` | 表示 indexed schedule 是否以 constant bitrate 工作。 |
| `*_initial_cpb_removal_delay_length_minus1` | 给出 initial CPB removal delay 的 bit length 减一。 |
| `*_cpb_removal_delay_length_minus1` | 给出 CPB removal delay 的 bit length 减一。 |
| `*_dpb_output_delay_length_minus1` | 给出 DPB output delay 的 bit length 减一。 |
| `*_time_offset_length` | 给出 signed time-offset bit length；零表示不存在 time-offset 语法。 |

type `8` 解码 clause 7.3.2.2 的 base PPS，要求只有一个 slice group，并支持有界的可选
High-profile extension。可见的 `has_pps_extension` computed field 会在 base 字段后调用
`more_rbsp_data()`，因此合法的 base-only High-profile PPS 不会被误认为截断的 extension。
存在 extension 语法时，精确 imported SPS generation 必须满足 `profile_idc == 100`；
Baseline、Main 与 Extended 码流会在读取任何 extension 字段前，于带 source 的
`seq_parameter_set_id` 上失败。

PPS/SPS identifier 或默认 reference-index count 越界时，会用字段 warning 保留完整 PPS。
非零 `num_slice_groups_minus1`、reserved `weighted_bipred_idc` 或
`pic_scaling_matrix_present_flag == 1` 会改变后续布局，因此成为 `invalid-syntax`。
materialized PPS 会解析该 PPS NAL 之前具有声明 ID 的最近 available SPS，并在发布自身
generation 时绑定这个精确 generation。missing 或 future SPS 会报告
`dependency-unavailable`，不发布 PPS，但不阻止分析后续 NAL。已经绑定并发布的 PPS 在新
合法 SPS generation 替换其 dependency 后，会对更晚的 consumer 变为不可用；此时由
consumer 报告 `dependency-unavailable`。下面的有界 type-1 与 type-5 slice header 也会使用
通用 context import；其余所有 type 的 `rbsp_payload` region 保持原样。

已声明 PPS 字段具有以下有界含义：

| 字段 | 本切片中的含义 |
| --- | --- |
| `pic_parameter_set_id` | 标识 PPS；clause 7.4.2.2 将其约束为 `0..255`。 |
| `seq_parameter_set_id` | 选择 PPS NAL 之前最近 available 的 SPS generation；clause 7.4.2.2 将其约束为 `0..31`。 |
| `entropy_coding_mode_flag` | 为关联 slice 选择 CAVLC 或 CABAC entropy coding。 |
| `bottom_field_pic_order_in_frame_present_flag` | 表示关联 slice header 中存在 bottom-field picture-order 语法。 |
| `num_slice_groups_minus1` | 必须为零，因为本切片不解析 flexible macroblock ordering。 |
| `num_ref_idx_l0_default_active_minus1` | 设置默认 list 0 reference count，超出 `0..31` 时告警。 |
| `num_ref_idx_l1_default_active_minus1` | 设置默认 list 1 reference count，超出 `0..31` 时告警。 |
| `weighted_pred_flag` | 为 P 与 SP slice 启用 weighted prediction。 |
| `weighted_bipred_idc` | 选择关闭、explicit 或 implicit weighted biprediction；值 3 为 reserved。 |
| `pic_init_qp_minus26` | 设置相对 26 的初始 luma QP；依赖 SPS 的 signed bound 留待后续。 |
| `pic_init_qs_minus26` | 设置相对 26 的初始 SP/SI QP；clause 7.4.2.2 将其限制在 `-26..25`，越界值只告警且不移动后续边界。 |
| `chroma_qp_index_offset` | 设置第一个 chroma QP index offset；clause 7.4.2.2 将其限制在 `-12..12`，越界值只告警且不移动后续边界。 |
| `deblocking_filter_control_present_flag` | 表示关联 slice header 中存在 deblocking-filter control 语法。 |
| `constrained_intra_pred_flag` | 将 intra prediction 限制在 intra-coded 相邻 macroblock。 |
| `redundant_pic_cnt_present_flag` | 表示关联 slice header 中存在 redundant-picture count 语法。 |
| `has_pps_extension` | 用非消费方式区分可选 extension 与 terminal RBSP pattern 的 computed Boolean；没有 source location。 |
| `transform_8x8_mode_flag` | 在有界 High-profile extension 中启用 8x8 transform decoding。 |
| `pic_scaling_matrix_present_flag` | 必须为零，因为 PPS scaling-list 语法仍属于 layout-critical unsupported。 |
| `second_chroma_qp_index_offset` | 设置第二个 chroma QP index offset；clause 7.4.2.2 将其限制在 `-12..12`，越界值只告警且不移动后续边界。 |

type `1` 为帧或场、reference 或 non-reference 形态下的有界 I、P 与 B slice 解码
`NonIdrSliceLayerWithoutPartitioningRbsp`。`NonIdrSliceType` 接受 I 值 2/7、P 值 0/5
与 B 值 1/6。三个可见 computed field 区分布局：`is_p_slice`、`is_b_slice`，以及对任意
P 或 B slice 为 true 的 `uses_reference_lists`。type-1 header 接受任意 reference
priority，structure 通过 `header_value(nal_ref_idc)` 读取该值以决定
`dec_ref_pic_marking()` 是否存在。

type-1 与 type-5 slice structure 都从精确 imported SPS generation 选择 picture-order
语法。POC type 0 保持既有 `pic_order_cnt_lsb` 与可选 bottom-field delta 的顺序。POC type 1
在归一化 always-zero flag 为一时不读取 slice delta；否则可见的 computed count 会选择
`delta_pic_order_cnt[0]`，并在 PPS 为帧图像启用 bottom-field-in-frame 语法时再选择
`delta_pic_order_cnt[1]`。POC type 2 在后续 slice-header 字段前不读取 POC 语法。

B slice 首先读取 `direct_spatial_mv_pred_flag`。随后每个 P 与 B slice 都会读取必需的
`num_ref_idx_active_override_flag`；值 1 会选择有界 `num_ref_idx_l0_active_minus1`
override，且仅 B slice 会紧接着读取 `num_ref_idx_l1_active_minus1`，值 0 则保留 PPS
default。后续 `ref_pic_list_modification_flag_l0` 仍是必需字段。值 0 不发布
modification field；值 1 进入有界 list 0 loop，其 operation code 选择 short-term
subtraction、short-term addition、long-term selection 或 termination。B slice 之后读取
`ref_pic_list_modification_flag_l1`；值 1 进入与 list 0 形状相同的第二个有界 loop。由于
structure 只有一个扁平字段命名空间，list 1 的投射名带 `_l1` 后缀；clause 7.3.3.1 对两个
loop 使用完全相同的名字，因此该后缀只用于呈现层消歧，并不表示另一个 syntax element。

可选 loop 之后是 clause 7.3.3.2 的 `pred_weight_table()`。当 P slice 使用
`weighted_pred_flag == 1` 的 PPS，或 B slice 使用 `weighted_bipred_idc == 1` 的 PPS 时
该表存在；这一条件由**单个** computed 存在性字段承载，因为互斥分支不能重名，嵌套会迫使
整张表复制两份。表先读 `luma_log2_weight_denom` 与 `chroma_log2_weight_denom`，然后按
list 0 的有效表项数循环——override flag 选中时用 slice 级的
`num_ref_idx_l0_active_minus1`，否则用 imported 的 PPS 默认值。每一项先读
`luma_weight_l0_flag`（guard 一对 weight 与 offset），再读 `chroma_weight_l0_flag`
（guard 两个 Cb 与两个 Cr 值）。B slice 按自己的有效次数对 list 1 重复整个循环。色度值
无条件存在，因为受支持子集内 ChromaArrayType 恒为 1。两个 loop 都以 32 项为界，与 count
字段已经携带的 `0..31` 界一致。由于 clause 7.3.3.2 对一个 chroma 元素循环两次，而单整数
`repeat` 形式永远是 sentinel 形式，四个色度字段带 `_cb` 与 `_cr` 后缀；list 1 的投射名
带 `_l1`，理由与 modification loop 的扁平命名空间相同。implicit biprediction 不读表，
保留的 `weighted_bipred_idc` 仍在读取它的 PPS 处保持 `invalid-syntax`。

NAL header 带非零 reference priority 的 slice 随后解码 clause 7.3.3.3 的
`dec_ref_pic_marking()`。`adaptive_ref_pic_marking_mode_flag` 为零时选择 sliding-window
marking，不再发布任何字段；为一时进入由 operation 零终止的有界 memory-management control
operation 循环，该 terminator 保留在树中。operation 1 与 3 读取
`difference_of_pic_nums_minus1`，operation 2 读取 `long_term_pic_num_mmco`，operation 3
与 6 读取 `long_term_frame_idx`，operation 4 读取 `max_long_term_frame_idx_plus1`，
operation 0 与 5 不读取 operand。保留 operation 在 controller 码字处致命失败，因为它不
对应任何 operand 组合，之后每个字段都会错位。只有 `long_term_pic_num_mmco` 带后缀，因为
clause 7.3.3.3 的 `long_term_pic_num` 与 list 0 modification loop 的投射名冲突；其余
operation 字段独有，保留 clause 名。non-reference slice 完全不发布 marking 字段。

当精确 PPS 启用 entropy coding 时，任意 P 或 B slice 随后会在 `slice_qp_delta` 之前读取
`cabac_init_idc`；超出 `0..2` 的值会告警，但不会改变剩余 header boundary。即使 entropy
coding 已启用，all-I 值也会短路全部 reference-list operation。IDR 专属的
`idr_pic_id`、`no_output_of_prior_pics_flag` 与 `long_term_reference_flag` 不出现——
clause 7.3.3.3 在 `IdrPicFlag` 路径上读取它们。

type `5` 为帧或场形态下的有界 all-I `slice_type` 值 2 和 7 解码
`IdrSliceLayerWithoutPartitioningRbsp`。它导入 `pic_parameter_set_id` 选择的精确 PPS
generation 以及该 PPS 的精确 SPS dependency。两个有界 shape 中声明的 slice 字段含义如下：

| 字段 | 本切片中的含义 |
| --- | --- |
| `first_mb_in_slice` | 标识 slice 中的第一个 macroblock。 |
| `slice_type` | type 1 接受 `p = 0`、`b = 1`、`i = 2`、`all_p = 5`、`all_b = 6` 与 `all_i = 7`；type 5 只接受两个 all-I 值。其他值在该码字处致命失败。 |
| `is_p_slice` | type-1 computed Boolean，值 0/5 时为 true；没有 source location。 |
| `is_b_slice` | type-1 computed Boolean，值 1/6 时为 true；没有 source location。 |
| `uses_reference_lists` | type-1 computed Boolean，任意 P 或 B slice 为 true；没有 source location。 |
| `pic_parameter_set_id` | 选择此前精确 PPS generation，超出 `0..255` 时告警。 |
| `frame_num` | 使用绑定 SPS 的 `log2_max_frame_num_minus4 + 4` bit。IDR slice 要求值为零；非零值告警但不移动后续边界。non-IDR slice 保留该宽度的完整值域。 |
| `field_pic_flag` | 选择场编码或帧编码；仅当绑定 SPS 清零 `frame_mbs_only_flag` 时存在。 |
| `bottom_field_flag` | 选择底场；仅当 `field_pic_flag` 为 1 时存在。 |
| `idr_pic_id` | 标识 IDR picture；超出 `0..65535` 时告警，但不改变后续字段边界。 |
| `pic_order_cnt_lsb` | 对 POC type 0，使用绑定 SPS 的归一化 `effective_log2_max_pic_order_cnt_lsb_minus4 + 4` 宽度。 |
| `has_delta_pic_order_cnt_bottom` | POC-type-0 computed Boolean；绑定 PPS 启用 bottom-field delta 且当前不是场图像时为 true，没有 source location。 |
| `delta_pic_order_cnt_bottom` | 对 POC type 0，在 `has_delta_pic_order_cnt_bottom` 为 true 时携带 signed bottom-field delta。 |
| `delta_pic_order_cnt_count` | slice delta 未被推断为零时存在的 POC-type-1 computed count；值为 1，或在 PPS 为帧图像启用第二个 delta 时为 2，没有 source location。 |
| `delta_pic_order_cnt[index]` | 携带 signed type-1 POC delta；computed count 存在时始终发布 `[0]`，仅当 count 为 2 时发布 `[1]`。 |
| `redundant_pic_cnt` | 在绑定 PPS 启用时标识 redundant representation；超出 `0..127` 时告警。 |
| `direct_spatial_mv_pred_flag` | 为受支持 B slice 选择 spatial 或 temporal direct prediction。 |
| `num_ref_idx_active_override_flag` | 受支持 P 或 B slice 的必需字段；值 1 读取 active-reference override，值 0 保留 PPS default。 |
| `num_ref_idx_l0_active_minus1` | 被选择时覆盖 active list 0 entry count；超出 `0..31` 时告警。 |
| `num_ref_idx_l1_active_minus1` | B slice 被选择时覆盖 active list 1 entry count；超出 `0..31` 时告警。 |
| `ref_pic_list_modification_flag_l0` | 受支持 P 或 B slice 的必需字段；值 1 选择有界 list 0 modification loop，值 0 不发布 loop field。 |
| `modification_of_pic_nums_idc[index]` | 选择 short-term subtraction (0)、short-term addition (1)、long-term picture number (2) 或 termination (3)；其他值致命失败。 |
| `uses_abs_diff_pic_num[index]` | operation code 为 0/1 时为 true 的 computed Boolean；没有 source location。 |
| `abs_diff_pic_num_minus1[index]` | 在 operation code 0/1 下携带 short-term picture-number difference operand。 |
| `long_term_pic_num[index]` | 在 operation code 2 下携带 long-term picture-number operand。 |
| `ref_pic_list_modification_flag_l1` | 受支持 B slice 的必需字段；值 1 选择有界 list 1 modification loop，值 0 不发布 loop field。 |
| `modification_of_pic_nums_idc_l1[index]` | `modification_of_pic_nums_idc` 的 list 1 对应项；`_l1` 后缀只用于呈现层消歧。 |
| `uses_abs_diff_pic_num_l1[index]` | list 1 operation code 为 0/1 时为 true 的 computed Boolean；没有 source location。 |
| `abs_diff_pic_num_minus1_l1[index]` | 在 list 1 operation code 0/1 下携带 short-term picture-number difference operand。 |
| `long_term_pic_num_l1[index]` | 在 list 1 operation code 2 下携带 long-term picture-number operand。 |
| `adaptive_ref_pic_marking_mode_flag` | NAL header 带非零 reference priority 时出现；值 0 选择 sliding-window marking，值 1 选择有界 operation 循环。 |
| `memory_management_control_operation[index]` | 在 `0..6` 中选择 marking operation；operation 0 终止循环，保留值致命失败。 |
| `marking_uses_pic_num_difference[index]` | operation 为 1/3 时为 true 的 computed Boolean；没有 source location。 |
| `difference_of_pic_nums_minus1[index]` | 标识 operation 1 与 3 所作用的 short-term picture；必须小于由帧/场形态推导出的 `MaxPicNum`。 |
| `long_term_pic_num_mmco[index]` | 标识 operation 2 要 unmark 的 long-term picture；必须小于 imported SPS `max_num_ref_frames`；带后缀是因为 clause 7.3.3.3 复用了 list 0 loop 的名字。 |
| `marking_uses_long_term_frame_idx[index]` | operation 为 3/6 时为 true 的 computed Boolean；没有 source location。 |
| `long_term_frame_idx[index]` | 为 operation 3 与 6 指派 long-term frame index；必须小于 imported SPS `max_num_ref_frames`。 |
| `max_long_term_frame_idx_plus1[index]` | 为 operation 4 设置 maximum long-term frame index 加一；不得超过 imported SPS `max_num_ref_frames`。 |
| `cabac_init_idc` | 为 entropy-coded P 或 B slice 选择 CABAC context initialization table；超出 `0..2` 的值会告警。 |
| `no_output_of_prior_pics_flag` | 控制 IDR picture 之前 picture 的输出。 |
| `long_term_reference_flag` | 设置时把 IDR picture 标记为 long-term reference。 |
| `slice_qp_delta` | 调整初始 luma quantization parameter；signed bound 留待后续。 |
| `disable_deblocking_filter_idc` | 在绑定 PPS 启用 control syntax 时选择 enabled、disabled 或 within-slice deblocking；reserved 值致命失败。 |
| `slice_alpha_c0_offset_div2` | 在 filtering 未禁用时携带 signed alpha/c0 deblocking offset；clause 7.4.3 将其限制在 `-6..6`，越界值只告警且不移动后续边界。 |
| `slice_beta_offset_div2` | 在 filtering 未禁用时携带 signed beta deblocking offset；clause 7.4.3 将其限制在 `-6..6`，越界值只告警且不移动后续边界。 |
| `slice_data` | 覆盖全部剩余 RBSP bit 的 materialized opaque suffix，其中包括可能存在的 slice trailing bits；不解码 CAVLC/CABAC。 |

imported SPS guard 会选择互斥的 type-0、type-1 与 type-2 POC 分支。只有 type 0 会计算
`pic_order_cnt_lsb` 的 dynamic width；type 1 使用归一化 always-zero flag 与有界 count 读取
零个、一个或两个 delta；type 2 不读取 POC 语法。reserved type 在 SPS 解码时失败。
exact imported PPS guard 会选择 redundant-picture count 与 deblocking-control
字段，type-0 bottom-field POC delta 改由 computed guard 选择；false guard 不消费 bit，也不创建
node。deblocking 值 1 省略两个 offset，值 0 和 2 读取
它们，reserved 值在 controller 码字处失败。missing/future/stale parameter-set generation 仍报告
`dependency-unavailable`；保留 partial header，并继续分析后续 NAL。每个 modification loop
与 marking loop 各自以 64 个 operation 受界，这是 sentinel repeat 的语言上界。它们是资源
边界，而不是宣称的 conformance limit。隔行序列（`frame_mbs_only_flag == 0`）会读取
`field_pic_flag`，场图像再读取 `bottom_field_flag`；即使其 PPS 启用了该字段，场图像
也会抑制 `delta_pic_order_cnt_bottom`。对 POC type 1，场图像同样会抑制
`delta_pic_order_cnt[1]`，SPS always-zero flag 则会抑制两个 type-1 delta。MBAFF 帧以相同的 header 布局被接受，因为宏块
自适应编码只改变不透明 `slice_data` 的解释方式。marking loop 现在会检查可由 imported
SPS `max_num_ref_frames` 表达的三条 per-operation bound：operation 2 的
`long_term_pic_num_mmco`、operation 3/6 的 `long_term_frame_idx`，以及 operation 4 的
`max_long_term_frame_idx_plus1`。operation 1/3 还会用由帧/场形态推导出的 `MaxPicNum`
约束 `difference_of_pic_nums_minus1`。SP/SI slice type、派生
`PicOrderCnt`/`TopFieldOrderCnt`/`BottomFieldOrderCnt`、`FrameNumOffset` 与 wrap state、
MMCO-5 effect、field pairing 与 output order、signed POC offset domain 与 cycle-sum 校验、
decoded-picture-buffer validation、operation 顺序/重复语义、权重施加语义、CABAC slice-data
解码与 slice-group 分支均留待后续。PPS scaling list 与依赖 SPS 的 `pic_init_qp_minus26` 和 `slice_qp_delta` domain 校验也留待
后续。
type 1 已由规则拥有，因此未派发 opaque fixture 改用 NAL type 12。package
`0.1.30` 发布 coverage depth `baseline-main-high-slice-header`，完成了
Baseline/Main/High 8-bit 4:2:0 单 slice group 的 slice header 里程碑，并将
slice data 整体作为不透明压缩载荷。

当码流中途重定义具有相同标识符的序列参数集或图像参数集时，后续 slice
按位置绑定到最近的先前有效代（ADR-0028、ADR-0078）。诸如动态 `frame_num`
宽度等上下文表达式根据新激活的代进行求值，而重定义之前的早期 slice 保持
绑定到先前的代。失败的参数集重定义不会发布新代，亦不会污染既有代。

Annex B analysis batch 除 record count 和 inspected-position budget 外，还使用独立且必须为正
的 mapped-byte budget；默认每次最多处理 64 KiB EBSP source byte。预算耗尽时返回
`in-progress`，并保留 mapper 已提交的前缀，后续 batch 可从同一个 NAL 继续。

mapper 把带 source 位置的 conformance issue 与 RBSP transformation 分开报告。对于
`00 00 03 xx` 且 `xx > 03`，仍排除 `03` 并报告该禁止序列；`00 00 00`、`00 00 01` 和
`00 00 02` 会被转发但同时报告；非空 payload 的最后一个字节为 `00` 时也会报告。此类 issue
保留完整的 `rbsp_payload` 与 excluded region，只把受影响的 NAL 标记为 invalid，并继续分析
后续 NAL。取消、source error 或 resource limit 失败会保留 direct header，按已提交的 RBSP
前缀发布对应 state 和 diagnostic，然后再终止当前 NAL 与 root。

最后一个空 NAL 仍会发布 NAL region 和 start-code 子节点，并发布一个没有字段的 invalid
`NalUnitHeader`；所属 NAL 的 `truncated-source` 汇总诊断锚定在已知 NAL region。
`@equals(0)` 不匹配时保留 `forbidden_zero_bit`，把 header 和所属 NAL 标记为 invalid，
但不阻止整体扫描完成。type-5 reference-priority assertion 同样保留完整 direct header，但会在 RBSP mapping 前停止，并把
diagnostic 锚定到 `nal_ref_idc`。header 读取失败时保留已发布节点，把 root 标记为 invalid，并返回
`source-error`；取消时保留已完成的 NAL region，并把 root 标记为 cancelled。

cancelled Annex B analyzer 可以用空 token 或尚未 requested 的新 cancellation token 原地恢复。
只有 cancelled terminal state 接受恢复。恢复会删除 root 的 cancellation diagnostic，把 root
转回 `indexing`，并保留 scanner cursor、queued record、tree、node ID 和下一个 NAL/view
identifier。若 cancellation 由 scanner 报告，扫描从其 pending boundary 继续；若在 NAL decode
或 mapping 中已经提交 cancellation，该 NAL 与 mapped prefix 保持 cancelled partial result，
不会重试，index 从后续 record 继续。到达 source 末尾后，即使 cancelled descendant 让 tree
保持 partial，root 仍会进入 `materialized`。Complete、source-error、resource-limit 与 invalid-rule
结果不能恢复；持久恢复需要后续 source fingerprint、精确 rule identity 与 durable cache storage。

最小合法示例：

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
    assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
}

@index(progressive)
sequence<NalUnitHeader> nal_units = scan(h264_start_code);
entry nal_units;
```

context publication 与 import 合法示例：

```cpp
@context("h264-sps", sps_id)
struct Sps {
    ue sps_id;
    ue log2_max_frame_num_minus4 @context_export;
}

@context("h264-pps", pps_id)
@context_dependency("h264-sps", sps_id)
struct Pps {
    ue pps_id;
    ue sps_id;
    bits<1> entropy_coding_mode_flag @context_export;
    bits<1> weighted_pred_flag @context_export;
}

@context_import("h264-pps", pps_id)
struct SliceHeader {
    ue first_mb_in_slice;
    ue slice_type;
    computed<bool> is_p_slice = slice_type == 0 || slice_type == 5;
    ue pps_id;
    assert(!is_p_slice ||
           context_value(pps_id, h264_pps, weighted_pred_flag) == 0)
        at pps_id;
    assert(!is_p_slice ||
           context_value(pps_id, h264_pps, entropy_coding_mode_flag) == 0)
        at pps_id;
    bits<context_value(pps_id,
                       h264_sps,
                       log2_max_frame_num_minus4) + 4> frame_num;
}

entry Sps;
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

unsigned Exp-Golomb enum 合法示例：

```cpp
enum IdrAllISliceType {
    i = 2;
    all_i = 7;
}

struct SliceHeaderPrefix {
    ue first_mb_in_slice;
    ue slice_type @enum(IdrAllISliceType);
}

entry SliceHeaderPrefix;
```

Exp-Golomb 合法示例：

```cpp
@spec("ITU-T H.264", "7.3.3")
struct SliceHeaderPrefix {
    ue first_mb_in_slice;
    ue slice_type;
    se slice_qp_delta @description("有符号 QP delta。");
}

entry SliceHeaderPrefix;
```

固定数组合法示例：

```cpp
enum SampleKind {
    luma = 1;
    chroma = 2;
}

struct Samples {
    bits<2> kinds[4] @enum(SampleKind);
    bits<16, little> values[2] @description("小端样本。");
    ue run_lengths[3];
}

entry Samples;
```

等值条件合法示例：

```cpp
enum PacketKind {
    compact = 1;
    extended = 2;
}

struct Packet {
    bits<2> kind @enum(PacketKind);
    if (kind == 1) {
        bits<3> compact_value;
    } else {
        bits<5> extended_value;
    }
    bits<3> tail;
}

entry Packet;
```

imported equality conditional 合法示例：

```cpp
@context("h264-pps", id)
struct Pps {
    ue id;
    bits<1> optional_present @context_export;
}

@context_import("h264-pps", id)
struct Slice {
    ue id;
    if (context_value(id, h264_pps, optional_present) == 1) {
        se optional_value;
    }
    bits<1> tail;
}

entry Slice;
```

等值 switch 合法示例：

```cpp
enum PacketKind {
    compact = 1;
    extended = 2;
}

struct Packet {
    bits<2> kind @enum(PacketKind);
    switch (kind) {
    case 1: {
        bits<3> compact_value;
    }
    case 2: {
        bits<5> extended_value;
    }
    default: {
        bits<4> unknown_value;
    }
    }
    bits<2> tail;
}

entry Packet;
```

有界 repeat 合法示例：

```cpp
struct SampleTable {
    bits<8> sample_count;
    repeat (sample_count, 16) {
        bits<16, little> value @description("小端样本。");
        bits<8> flags[2];
    }
}

entry SampleTable;
```

bounded sentinel 合法示例：

```cpp
enum ModificationOfPicNumsIdc {
    subtract_short_term = 0;
    add_short_term = 1;
    long_term = 2;
    end = 3;
}

struct RefPicListModifications {
    repeat (64) {
        ue modification_of_pic_nums_idc @enum(ModificationOfPicNumsIdc);
        computed<bool> uses_abs_diff_pic_num =
            modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1;
        if (uses_abs_diff_pic_num) {
            ue abs_diff_pic_num_minus1;
        }
        if (modification_of_pic_nums_idc == 2) {
            ue long_term_pic_num;
        }
    } until (modification_of_pic_nums_idc == 3);
}

entry RefPicListModifications;
```

纯函数与计算字段合法示例：

```cpp
pure bool between(u64 value, u64 low, u64 high) {
    return value >= low && value <= high;
}

struct NalUnitHeader {
    bits<5> nal_unit_type;
    computed<bool> is_vcl = between(nal_unit_type, 1, 5)
        @description("视频编码层 NAL unit。");
    computed<u64> next_type = nal_unit_type + 1;

    if (is_vcl) {
        bits<1> first_slice_flag;
    }
    switch (next_type) {
    case 6: {
        bits<1> follows_vcl_range;
    }
    }
}

entry NalUnitHeader;
```

payload 派发合法示例：

```cpp
@spec("ITU-T H.264", "7.3.1")
struct NalUnitHeader {
    bits<1> forbidden_zero_bit @equals(0);
    bits<2> nal_ref_idc;
    bits<5> nal_unit_type;
    assert(nal_unit_type != 5 || nal_ref_idc != 0) at nal_ref_idc;
}

@spec("ITU-T H.264", "7.3.2.4")
struct AccessUnitDelimiterRbsp {
    bits<3> primary_pic_type;
    rbsp_trailing_bits;
}

@index(progressive)
sequence<NalUnitHeader> nal_units = scan(h264_start_code);

@spec("ITU-T H.264", "7.3.1")
payload<rbsp> nal_units switch (nal_unit_type) {
    case 9:  AccessUnitDelimiterRbsp;
    case 10: empty;
    case 11: empty;
}

entry nal_units;
```

最小非法示例包括 `bits<0> flag;`、`bits<65> flag;`、`bits<12, little> value;`、
位于未对齐字段之后的小端字段、`se value @equals(0);`、`se value @equals(-1);`、
`se value @enum(Type);`、
`bits<4> value @range(0, 16);`、`se value @range(6, -6);`、
`se value @range(0, 9223372036854775808);`、
`se value @range(-9223372036854775808, 0);`、`ue value @range(-1, 12);`、
`bits<4> value @range(-1, 4);`、
`computed<u64> value = 1 @range(0, 12);`、`ue value @range(12, 0);`、
`ue value @range(0);`、`ue value @range(0, 12) @range(0, 6);`、
变长字段之后的小端字段、`bits<1> flags[0];`、`bits<1> flags[];`、数组长度表达式或第二
维、展开后超过 99,999 字段的结构、截断的数组元素、截断的 Exp-Golomb 码字、64 个前导零、`@enum(Missing)`、
无法放入字段宽度的 enum member 值、`Type` 含 `18446744073709551615` 时的
`ue value @enum(Type)`、重复 enum member 名、缺少 `@index(progressive)` 的
sequence、在 `future` 声明前使用 `if (future == 1)`、以数组或 `se` 作为条件
controller、条件整数超出 controller 宽度、离开分支后使用 branch-local controller、
`if (flag = 1)`、使用未来字段、数组或 `se` controller 的 switch、超出宽度或重复的
case 值、没有 case 的 switch、重复或不位于最后的 default、缺少冒号或花括号 body 的 case、
`break`、fallthrough、同一 arm 的多个 label、case 范围或 enum member label、
使用未知、未来、数组、`se` 或路径上不可用的 branch-local controller 的 repeat、
`repeat (count, 0)`、不能由定宽 controller 表示的 maximum、计数或 maximum 表达式、空
repeat body、repeat 前的 annotation、投影后超过 99,999 字段、在其他迭代或 repeat 之后
使用 repeat-local controller、repeat 之后的小端字段、`scan(other_scanner)`、重复声明同名，
以及没有 `entry` 或包含多个 `entry` 声明的程序。
bounded-power 的非法示例包括 `power_of_two()`、`power_of_two(1, 2)`、Boolean exponent，
以及命名为 `power_of_two` 的 pure function，因为该 expression name 已保留。exponent 为 64
或更大值时类型仍合法，但会在求值时失败。
RBSP source-state 的非法示例包括 `more_rbsp_data(1)`、把 Boolean 结果赋给
`computed<u64>`、在 pure-function body 中使用它，或声明名为 `more_rbsp_data` 的 pure
function。
sentinel repeat 的非法示例包括 `repeat (0)` 或 `repeat (65)`、缺少 `until` clause、未知
sentinel、sentinel 声明在 body 外或 nested control flow 内、以 array、`se`、dynamic-width、
computed 或 lazy 项作为 sentinel、termination value 越界，以及使用直接等于整数字面量之外的
任何比较。

assertion 的非法示例包括：condition 不是 Boolean；dependency 或 anchor 未知、声明得更晚、是
array，或在当前 path 不可用；以 `se` 作为 expression dependency；以 computed、generated、
lazy、compressed 或 array field 作为 anchor；在 conditional、switch 或 repeat control flow 内
声明 assertion；任何前置 annotation；缺少括号、`at`、anchor 或分号；malformed
`context_value` argument list、无法解析的 imported descriptor，或直接把 imported `u64` 当作
condition；以及一个 structure 中的第 1,025 条 assertion。

context publication 的非法示例包括：同一 structure 上出现第二个 `@context`、完全相同的
`@context_dependency` 重复、没有 `@context` 的 structure 声明 dependency、使用 guarded、
repeated、array、signed、lazy 或 generated field 作为 key/export、
`@context_export(1)`、超过 16 个 dependency，以及超过 64 个 export。未知 context kind 与
不存在的 key 名同样会被拒绝。
context import 的非法示例包括：完全相同的 import 重复、guarded/repeated/array/signed/lazy/
generated 或未知 key field、不支持的 kind、malformed argument，以及超过 16 个 import。
imported dynamic-width 的非法示例包括：在 dynamic `bits` width、imported equality
conditional、source-anchored assertion condition 或 computed field initializer 之外使用
`context_value`；import key 缺失
或声明得更晚，包括所在 structure 未声明匹配 import 的 computed initializer；
target kind 与 import closure 无关；target kind 没有 publisher 或存在多个
publisher；export 缺失；以及 dynamic little-endian、array、enum、constrained、context-key、
dependency、import 或 export field。runtime 结果为 `0` 或 `65`、arithmetic
overflow/underflow、除零或模零时，在该字段消费输入前报告 `invalid-syntax`。
imported-condition 的非法示例包括：在 `context_value` 外包 arithmetic 或 call、`!=`、
ordering、Boolean combination、imported Boolean shorthand、非 literal 右侧、缺失或较晚声明的
import key、无关 target kind、零或多个 publisher，以及缺失 export。imported value 仍不能作为
switch/repeat controller、sentinel condition、lazy expression、array length、
annotation 或 payload dispatch value。imported count 只能以 `computed<u64>` 字段的形式抵达
repeat controller，绝不能在 controller 位置直接写成裸的 imported value。
sequence-element 的非法示例包括：`header_value` 带零个、两个或更多实参；非 identifier
实参；未知的 element 字段名；guarded、位于 repeat 中、数组、dynamic-width 或 signed 的
element 字段；没有声明 sequence 的程序；写在 sequence element structure 自身内部的调用；
以及出现在 pure-function body、lazy size、controller 或任何已经拒绝
`context_value` 的位置。
optional-value 的非法示例包括：`optional_value` 带一个、三个或更多实参；第一个实参不是
identifier；第一个实参是未知字段、更晚声明的字段、数组、`se`、`computed<bool>`，或已经
离开 scope 的 repeat body 字段；fallback expression 为 Boolean；fallback 的 dependency
本身是 branch-local；以及出现在 pure-function body、condition、switch controller，或
imported 与 sequence-element 等值 conditional 中的调用。

payload 派发的非法示例包括：两个 payload 声明、`payload<ebsp>` 或其他 view kind、派发命名
结构或未声明名称而非 sequence、所派发 sequence 没有 `entry`、未知 controller 名、以
Exp-Golomb、计算字段、数组元素或 lazy region 作为 controller、controller 声明在 conditional、
switch 或 repeat body 内部、case 值超出 controller 宽度、重复 case 值、没有 case 的派发、
未声明或就是 element structure 本身的 case 目标、`default` arm，以及缺少 case 冒号或分号。

纯函数与计算字段的非法示例包括：带 annotation 的纯函数、超过 16 个参数、重复参数名或
函数名、overload 或顶层名称冲突、纯函数体调用后声明的函数或发生递归、在纯函数体内引用
非参数值、返回
类型或 call argument 不匹配，以及 malformed 或缺少 return expression。还包括 computed
数组、`computed<se>`、计算字段上的 `@equals` 或 `@enum`、引用数组、`se`、未来、未知或
路径上不可用的 branch-local 字段、混合类型 operator、不支持的 operator 或转换、超过
256 nodes、depth 64 或 4,096 个共享 expansion work step 的计算表达式或展开 pure call、
在等值条件中使用 `computed<bool>` 或把它用作 switch/repeat controller、在 Boolean
`if (flag)` 简写中使用 `computed<u64>`，以及直接在数组长度、case label、repeat maximum
或 repeat controller 中使用
一般表达式。执行路径抵达的无符号 overflow/underflow、除零或模零属于 runtime
`invalid-syntax`；被 short-circuit 的失败 operand 不会求值。

malformed repeat header 会产生带 source range 的 missing-token 诊断；缺少 body 左花括号时，
parser 在下一个字段分号或外层右花括号处恢复，其他 header token 缺失时则尽可能继续解析
仍可识别的花括号 body。enum 和字段解析在下一个 member/field 分号或右花括号处恢复；遇到
未知 switch label 或缺少 arm 左花括号时，在下一个 `case`、`default` 或 switch 右花括号处
恢复。malformed pure、parameter、return、call、expression 或 `computed<...>` 语法同样
产生带 source range 的诊断。statement recovery 在下一个可用分号或外层右花括号处停止；
expression recovery 还会把当前 call 的逗号或右括号视为边界。所有恢复都会保留 source
range 和诊断。

## 源坐标与逻辑坐标

未经修改的媒体源使用绝对源坐标。逻辑视图拥有自己的逻辑坐标，同时保存经过所有父视图返回绝对源区间的有序映射。一个语法字段可以映射到多个不连续的源区间。

字节序属于数值解释规则，不属于坐标规则。显式 `little` 因此不会改变逻辑范围、绝对
source spans、selection 或诊断位置；这些坐标与默认大端读取完全相同。

选择语法字段时，高亮它映射到的全部源区间；选择原始 bit 时，定位到当前已经物化的
最具体节点，并保留它在分析树中的完整父级路径。解析顺序按
[分析模型](../analysis-model.md#source-bit-定位)定义的树深度、source 覆盖长度和稳定
节点 ID 确定。
计算字段没有 `FieldLocation`，不参与 source-bit resolution，只能通过其 analysis-tree 节点
选择。

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

规则必须显式声明可以延后物化内容的安全边界。当前接受的 lazy-byte 切片会在不读取
内容的情况下注册一段未解释的 logical-byte range：

```cpp
struct Packet {
    bits<16> payload_size;

    @lazy(payload_size)
    bytes payload @description("Deferred packet payload");
}
entry Packet;
```

只有 declaration 的全部 guard 都被选中时才会计算 expression。VM 在追加 region 或移动
cursor 前检查 byte-to-bit conversion、logical alignment、enclosing reader limit、
mapping resolution 和 node budget；它不会读取注册的 payload。正长度 range 状态为
`lazy`，空 range 会立即 `materialized`。location 包含精确 mapped logical range，也可以
包含多个 source span。

静态拒绝的例子包括：Boolean byte count、future 或 branch-unavailable dependency、在
alignment 未知的 variable-width field 后声明 lazy region、脱离 `@lazy` 的
`bytes payload;`，以及 lazy region 上的 `@equals` 或数组后缀。runtime arithmetic
overflow 报告 `invalid-syntax`；range 超过 enclosing reader 时报告 `truncated-source`，
且不会创建 lazy node 或移动 cursor。

lazy 切片只注册经过检查的 boundary，尚不接受 nested typed content、decode recipe 或用户
触发的 subtree expansion。当前接受的 progressive index 会在有界 batch 中发布 structure，
并能在同一个 analyzer 内恢复 cancelled scan；唯一 progressive form 是 H.264 start-code
sequence：

```cpp
@index(progressive)
sequence<NalUnit> nal_units = scan(h264_start_code);
```

analysis model 区分 lazy、indexing、cancelled、unsupported、invalid 和完全 materialized
等状态。

index recovery 保持 append-only tree，不会在后续 batch 重放已经发布的 node。它只删除承载
entry sequence 的 analysis root 上已经过时的 cancellation diagnostic。已经作为 cancelled
partial result 提交的 NAL 仍保持 cancelled，因此后来进入 `materialized` 的 root 仍可能属于
含 partial result 的 tree。这种内存恢复不是 serialized 或 cross-process checkpoint 契约。

## payload 派发

sequence element 解码出一个 header，紧随其后的语法通常取决于该 header 刚刚产出的某个值。
payload 派发把这些值绑定到解码派生 payload view 的结构：

```cpp
@spec("ITU-T H.264", "7.3.1")
payload<rbsp> nal_units switch (nal_unit_type) {
    case 9:  AccessUnitDelimiterRbsp;
    case 10: empty;
    case 11: empty;
}
```

该声明位于顶层，至多出现一次。`rbsp` 是当前唯一接受的 view kind，指 runtime 已经为每个
sequence element 派生的 mapped payload view。声明之前的 annotation 成为该派发自身的 metadata。

派发不新增 opcode。被选中的结构由 compiler 为每个已声明结构生成的同一套 `begin-structure`
到 `end-structure` bytecode 执行，因此 case 目标没有特殊的 typed 形式。case 互异性、
controller 解析和目标索引都在执行前完成校验；malformed 派发属于 invalid typed definition。

运行时在 header 物化之后，从 element 已发布的 header 中读取 controller 值。

未列出的值不改变任何行为：payload 非空时仍是未解释 region，没有 payload 的 element 也不会
获得 payload node。

已列出的值一定会获得派生 payload view，view 为空时也不例外。决定 view 是否存在的是「是否
存在 case」，而不是 payload 长度。描述了某个 payload 的规则因此总能拿到一个精确的 view 去
解码。

`empty` case 要求 payload view 的 logical length 恰好为零。非空 payload 在 payload path 上
报告 `invalid-syntax`，并保留完整的 payload region 与全部 excluded region。

结构 case 从 logical 零开始在 payload view 上执行其目标，父节点为 payload region node，
并沿用与 header 相同的 execution option、沙箱预算和取消检查点。

已物化的结构必须消费 payload view 的完整 logical length。剩余 bit 在 payload path 上报告
`invalid-syntax`。精确消费正是让 trailing-bit 声明可校验的原因，也阻止静默接受任何声明都
没有描述的字节。

所需 bit 多于 view 容量的结构报告 `truncated-source`，view 为空时同样如此。在内置 H.264
规则下，只有 header 的 access unit delimiter 因此是截断，而只有 header 的 end of sequence
是物化。

payload 失败使所属 element 变为 invalid 或 cancelled，同时保留 header、payload region、
excluded region 和任何 framing region。它绝不终止 sequence，后续 element 继续分析。

## 位置感知上下文目录

`RuleExecutionSession` 拥有一个精确 compiled program、一个格式中立的 context directory，
以及一个 analysis source/tree 发布的 rules-owned typed payload。首次有效执行会锁定 analysis
identity；用另一个 source 或 tree 复用属于 invalid rule/runtime state。移动 session 或其所属
analyzer 会保留 identity 与已发布 generation。当前接受的 kind 包含 H.264 SPS/PPS、AAC
AudioSpecificConfig 和 ISO BMFF sample description。key 还包含数字 scope，防止不同 track
中相同 sample-description 或 parameter-set value 冲突；standalone Annex B 使用 scope zero。

session 可以执行完整 logical view，也可以执行从非零 logical start 开始的 suffix。reader 只由
该 mapped slice backing，source location 保持原 logical coordinate；精确消费指消费该 slice，
不包含 mapping prefix。对于 context definition 或 import，这个 slice 的每个 mapped source
span 都必须位于非空 enclosing source span 内；不匹配会在读取 source 或绑定 analysis identity
前被拒绝。没有 context annotation 的结构仍走同一 execution path，但不产生 directory effect。

compiler 把每个声明的 key、dependency、export 与 import key 降低为稳定 typed field index。
VM 在读取 source 前验证这些 index 和类型，并且只返回选中值及其精确 source location，而不
暴露完整 local environment；session 与 analyzer 都不会遍历 presentation-tree child 来恢复
运行值。

definition 只有在查询 source position 到达或越过其完整 source span 的排他结束
位置时才可选。同一 key 的多个可选 definition 中，lookup 选择结束位置最接近
查询位置的一项。注册可以不按 source 顺序，但同 key span 不能重叠，稳定
definition ID 仍按追加顺序分配。

dependent definition 会绑定在自身 source span 开始前选中的精确
dependency-generation ID。在 consumer 位置，每个 dependency 都必须仍解析到同一
generation。后续重定义会让 dependent context unavailable；runtime 不会退回所请求
key 的更旧 generation，也不会猜测。malformed definition 不注册，被拒绝的注册是
事务式的；
后续跨 generation dependency cycle 会产生 dependency-unavailable result，超过 64 个
definition 的 dependency chain 也会得到同一结果。

dynamic width 首次请求 imported value 时，import 会在 consumer enclosing span 的起点解析；
如果没有更早的 value request，则在 consumer materialize 后解析。该精确 closure 会在本次 run
内缓存，并在成功精确消费后作为同一 closure 返回。缺少、future 或 stale generation 会在
import key 上添加精确 source 位置的 `dependency-unavailable` diagnostic，且不返回部分
imported result。成功 import 先返回 root definition，再按 dependency declaration order 做
depth-first traversal；每个精确
definition 只包含一次。每项保留 definition ID、kind、publishing structure index、有序 exported
value 与精确 dependency ID。closure 最多 64 个 definition；rules-owned payload 缺失属于
invalid runtime state。import result 不创建 analysis node。imported value 只能通过 dynamic
width 中 lower 后的 `context_value` leaf、精确的 `context_value(...) == integer` conditional、
source-anchored assertion condition，或 computed field initializer 使用。它不能作为一般
expression、lazy size、
switch/repeat controller 或 repeat bound 中的 identifier。由 imported value 初始化的
`computed<u64>` 本身可以控制 count repeat，这也是 imported entry count 抵达 repeat 的唯一
方式；import 在该计算字段求值时解析一次。

被派发的 payload structure 通过 `header_value(element_field)` leaf 读取自己所属的 sequence
element header。该 leaf 降低为 typed sequence-element reference，携带已解析的 element field
index，且不新增 opcode。runner 随 execution request 提供已解码的 element 字段值——这之所以
可行，是因为它在选择 case 之前本来就会物化 element header 并从中读出 dispatch controller。
虚拟机在读取 source 之前验证 descriptor：index 越界、值缺失或值向量缺席都是 invalid
definition，而不是用猜测值解码。位于此类 leaf 之上的 false guard 不消费 source，也不创建
node。详见
[ADR-0063](../adr/0063-read-sequence-element-fields-from-dispatched-payloads.md)。

两个保留 leaf 同样会在 computed field initializer 内求值，复用 assertion 与 dynamic-width
位置已经使用的同一 resolver 和 element value vector，因此计算字段不新增 opcode，也不引入上述
之外的新失败模式。详见
[ADR-0065](../adr/0065-admit-reserved-external-leaves-in-computed-initializers.md)。

`optional_value(...)` leaf 被降低为 typed optional field reference，携带解析出的字段
index，并以编译后的 fallback 作为唯一 operand，不新增 opcode。虚拟机本来就按每次
structure 执行记录字段是否存在，因此求值时：实际执行的路径物化了该字段就读取已记录的值，
没有物化就求值 fallback。guard 为假的字段即为不存在；repeat body 在 repeat 结束后离开
scope，因此不会有 index 与后续 projection 混淆。fallback 只在需要时才求值，其中的任何
失败都按包含该 leaf 的 position 原本的方式上报。详见
[ADR-0066](../adr/0066-select-an-optional-field-value-with-a-declared-fallback.md)。

`power_of_two(...)` leaf 降低为一条 typed expression node，并在外层 instruction 内求值。
exponent 会在 shift 前检查，因此 malformed typed descriptor 不会触发未定义的 host shift。
详见 [ADR-0070](../adr/0070-add-bounded-power-of-two-expression.md)。

`more_rbsp_data()` leaf 降低为零 operand 的 Boolean typed-expression node，并在外层
instruction 内求值。VM 探测当前 reader 的副本，因此查询成功或 source 读取失败都不会改变
执行 cursor。详见
[ADR-0072](../adr/0072-observe-remaining-rbsp-data-in-expressions.md)。

只有 structure 成功 materialize、满足请求的精确消费策略、完成 dependency resolution 和
typed-payload 准备后才会发布。registration 前会预留 payload 与 directory 容量，因此成功
mutation 后提交 prepared payload 是 single-writer 模型下不分配内存的 move。malformed、
truncated、cancelled、resource-limited、dependency-unavailable 或残留 bit 的执行均不发布；
失败的 redefinition 因而不会隐藏此前 valid generation。

目录只持有 key、span、analysis-node 和 dependency identity，typed format payload 仍由
rule owner 保存。目录不读取 source，并遵循 analysis-worker 单写者模型。详见
[ADR-0028](../adr/0028-resolve-context-generations-by-source-position.md)。
publication 合同见
[ADR-0044](../adr/0044-publish-rule-declared-context-generations.md)。内置 H.264 rule 从 package
version `0.1.7` 开始使用该合同。import 合同见
[ADR-0045](../adr/0045-import-rule-declared-context-generations.md)；内置 rule 会在新增 slice
dispatch 后才使用 import。dynamic imported width 合同见
[ADR-0046](../adr/0046-evaluate-dynamic-bit-widths-from-imported-context-values.md)。
imported equality guard 合同见
[ADR-0052](../adr/0052-guard-fields-with-imported-context-values.md)。
有界 post-tested sentinel repeat 合同见
[ADR-0047](../adr/0047-lower-bounded-sentinel-repeats-to-guarded-projections.md)。
消费剩余 bit 的 compressed terminal 合同见
[ADR-0048](../adr/0048-register-a-compressed-remaining-bit-payload-terminal.md)。
source-anchored assertion statement 合同见
[ADR-0054](../adr/0054-add-source-anchored-assertion-statements.md)。
repeat-local assertion projection 合同见
[ADR-0069](../adr/0069-add-repeat-local-assertions-for-bounded-relations.md)。
source-anchored assertion 中 imported value 的合同见
[ADR-0056](../adr/0056-allow-imported-context-values-in-source-anchored-assertions.md)。
有界 progressive non-IDR all-I slice 合同见
[ADR-0055](../adr/0055-add-bounded-progressive-non-idr-all-i-slice-header.md)。
有界 progressive non-IDR P-slice 合同见
[ADR-0057](../adr/0057-add-bounded-progressive-non-idr-p-slice-header.md)。
有界 P-slice reference-index override 合同见
[ADR-0058](../adr/0058-add-bounded-p-slice-reference-index-override.md)。
有界 P-slice reference-list modification loop 合同见
[ADR-0059](../adr/0059-add-bounded-p-slice-reference-list-modification-loop.md)。
有界 P-slice CABAC initialization branch 合同见
[ADR-0060](../adr/0060-add-bounded-p-slice-cabac-initialization-branch.md)。

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

一个 structure 最多声明 16 个 context import。import selection 不读取 source，也不创建 node。
每个返回的 exact dependency closure 最多包含 64 个 definition；超限得到 `resource-limit`，
且不暴露部分 imported result。

dynamic-width imported leaf 复用这份 per-run closure，不增加 instruction、source read 或
presentation node。完整展开的 width expression 仍受 256-node 与 depth-64 上限约束，并在所选
字段的单条 read instruction 与既有 cancellation boundary 内求值。

数组语法不另占运行时预算。每个展开元素消耗一个物化节点和一条 read instruction；每个
`@equals` 元素再增加一条 assertion instruction，每个 `@range` 元素再增加两条
assertion instruction。因此截断、约束失败、指令上限或节点上限
都可能发生在元素之间，同时保留失败前已经完成的元素。静态的 99,999 字段投影上限确保
一次默认结构物化不会需要超过文档规定的 100,000 个节点。

`@range` 的两条 assertion instruction 计入 instruction budget 并且是取消检查点，但两者
都不新建 analysis node、不消耗 source bit，也不改变 bit reader position。因此违反 range
不会改变后续字段的读取位置或预算账目。

条件和 switch 语法都不另占 opcode 或节点预算。每个可能字段仍生成 read instruction，
`@equals` 仍生成 assertion instruction；即使字段被跳过，这些指令仍计入 instruction
budget，并保留取消检查点。只有选中字段消耗 source bit 和一个物化节点名额；全部分支和
switch arm 的字段都计入静态 99,999 字段投影上限。

每个投影后的 repeat 语句增加一条 `assert-repeat-count` instruction。按声明的 `maximum`
产生的每条 read 和 equality assertion 都计入 instruction budget，并且即使所属迭代缺席也
保留取消检查点。边界断言本身同样计费；即使外层 guard 为 false，它仍保留取消检查点。
只有存在的迭代消耗 source bit 和物化节点名额。全部投影字段都计入静态 99,999 字段上限，
因此保守的 maximum 即使面对较小的解码计数，也会增大 typed program 和可能执行的指令量。
执行路径抵达的计数超过 maximum 时报告 `invalid-syntax`，不是 `resource-limit`。

每个 sentinel repeat 在全部 projected field instruction 后增加一条
`assert-sentinel-terminated`。被跳过 projection 的 field instruction 与 cancellation point 仍计入
预算，但不消费 source bit 或 node；enclosing guard 为 false 时 assertion 也计费。若没有任何已选
sentinel 等于 termination value，assertion 会在最后一个 sentinel field 上返回
`invalid-syntax`，并保留有界 materialized prefix。language-wide maximum 64 同时限制 descriptor、
guard 与 assertion work。

一个 structure 在 repeat projection 后最多包含 1,024 条 source-anchored assertion。每条 assertion
增加一个 descriptor 和一条 `assert-expression` instruction；该 instruction 消耗一个
instruction-budget unit，即使 active conditions 使其跳过求值也仍是
cancellation point。完全内联的 condition 在这一条 instruction 内计算，仍受 256 个 expression
node、深度 64 与编译期 4,096-step expansion 上限约束。assertion 不增加 presentation node 或
source read，也不计入 99,999-field projection。repeat-local descriptor 保留 active field
conditions；VM 会先检查这些条件，再要求当前 iteration 的 anchor range 并求值 Boolean
condition。

每个计算字段增加一条 `evaluate-computed` instruction。即使 false guard 跳过求值，该
instruction 仍计入 instruction budget，并保留取消检查点。成功求值消耗一个物化节点名额，
但不消耗 source bit；失败求值不消耗节点名额，并保留此前节点。pure call 由 compiler
内联，因此不增加 runtime instruction。全部子表达式工作计入这一条 instruction，不新增
取消检查点，并受展开后 256-node 与 depth-64 上限约束。计算字段也计入静态 99,999 字段
投影上限。

每个投影后的 lazy byte region 增加一条 `register-lazy-bytes` instruction。即使 false guard
跳过它，该 instruction 仍计入 instruction budget 并保留取消检查点。选中的 declaration 在
这一条 instruction 内计算已经受限的 expression；只有完整 mapped boundary 通过检查后才
消耗一个 node 名额。seek 越过 region 不执行 source read。lazy region 计入静态 99,999-item
projection limit。

每个 compressed payload 增加一条 `register-compressed-payload` instruction、一个
materialized-node slot 与一个 cancellation point。该 instruction 会映射并 seek 越过 reader 的
完整剩余 range，但不读取 source；terminal 作为一个 item 计入静态 99,999-field projection
limit。

恢复 cancelled progressive index 本身不消耗 source work、node 或 batch budget。后续每轮
batch 继续使用普通 batch 相同的正 record-count、inspected-position、mapped-byte limit 和
cancellation interval。

所有限制都必须大于零。host 可以为一次执行降低限制，但规则本身不能提高或读取限制。
当前最小子集没有 runtime call 或 view：pure call 会被静态内联；未来加入 runtime call 或
view 时必须消耗已经保留的深度预算。超过指令、节点数量或节点深度限制时报告
`resource-limit`，保留超限前已完成的节点，并把当前结构标记
为 invalid。取消时报告 `cancelled`，保留已完成节点，并把当前结构标记为 cancelled；如果
取消发生在 `begin-structure` 之前，则标记其 parent。非法或损坏的 typed bytecode 会作为
invalid definition 被拒绝，运行时不会猜测执行。

输入输出、内存和 wall-clock 的精确默认值仍是暂定设计；使用这些预算的语言功能进入稳定
状态前，必须补齐对应契约。

## 规则包

规则包具有版本，并声明格式身份、引擎兼容范围、适用性元数据和依赖。格式探测只推荐候选规则，用户始终可以手动覆盖。分析会话保存最终采用的精确 package ID、version、content hash 与 entry-point ID。

应用、DSL 语言和规则包分别独立版本化。规则包清单声明精确的语言契约和引擎兼容范围。在 DSL `0.x` 阶段可以进行有文档记录的破坏性修改；语言进入 `1.0` 后，不兼容修改必须使用新的语言 major 版本。引擎遇到不兼容规则包时必须拒绝加载并报告诊断，不能猜测执行。

首版规则包自包含，显式声明空 dependency list，分析时不能通过网络解析依赖。package metadata
不会改变 sandbox 权限，也不建立 trust。

官方规则与特定应用版本一起发布。首版只允许从本地文件或目录安装其他规则包，不提供在线市场、自动下载或自动更新。安装前显示包身份、版本、格式覆盖、作者元数据、内容哈希和兼容范围。已保存会话锁定完整 package 与 entry-point identity；规则不会因为作者或“可信”声明而获得额外权限。

### 规则包结构

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

本地安装时，可以把目录编码成确定性的 store-only ZIP32 容器，扩展名固定为 `.svrule`。
version 1 `rule.toml` manifest 声明 package、作者、license、package version、精确 language
contract、半开 engine compatibility range、entry point、format/profile/depth coverage、host
detector metadata 与本地化文档。不同 package version 可以并存；同一个 package ID 与 version
不能静默表示不同 content。

发布的规则包不能包含原生可执行代码或 symbolic link。installer 必须拒绝 absolute path、parent
traversal、重复或非 canonical path、Unicode/case-fold alias、特殊文件以及任何逃逸 package root
的 entry。通过验证的 content 按完整 logical package tree 的 SHA-256 只读保留，因此目录与
archive 形式共享一个 identity。精确 schema、identity framing、catalog 行为、canonical archive
record 与限制由规范性的[规则包格式](../../rule-packages.md)定义。
