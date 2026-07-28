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

最终参考文档必须分别定义基本类型、有无符号、字节序、bit 顺序、溢出行为、数组、枚举、
结构、条件、分支、有界循环、纯函数、作用域、名称解析和规范注解。当前接受的最小子集
仍有明确边界：表达式只能出现在纯函数返回值、计算字段和 lazy byte count 中，控制流只
包含下述条件、switch 和有界 repeat 形式。

当前接受的 M3 类型切片新增了按声明顺序保存的 enum，以及 `bits` 字段的显式字节序。
enum 声明为无符号整数命名；字段通过 `@enum(Type)` 把这些名称关联到解码值。
字节序只改变数值解释，source bit 地址仍是 MSB-first，字段 source location 仍对应实际消耗的 bit。

当前接受的变长 primitive 切片新增了 H.264 风格的无符号和有符号 Exp-Golomb
字段，类型关键字为 `ue` 和 `se`。这两种类型没有显式宽度或 endian 参数；其 source
location 覆盖完整编码码字，而不是固定 bit 数。

当前接受的固定数组切片允许在 scalar 字段名后写一维正整数长度，例如
`bits<8> payload[4]`、`ue codes[3]` 或 `se deltas[3]`。compiler 把声明展开为独立完成
类型检查和执行的 `payload[0]` 到 `payload[3]`；不会新增数组值、容器节点或数组专用 opcode。

当前接受的条件切片新增了可嵌套的
`if (previous_field == integer) { ... } else { ... }` block，`else` 可以省略。
这个比较只是基于此前 scalar 定宽值的受限 presence 判断，不是一般表达式。

当前接受的 switch 切片新增了可嵌套的
`switch (previous_field) { case integer: { ... } default: { ... } }` block。
每个 arm 都有自己的花括号 body，`default` 可以省略，选择逻辑复用条件切片的受限等值
guard。

当前接受的有界 repeat 切片新增了可嵌套的
`repeat (previous_count, maximum) { ... }` block。此前解码出的无符号计数选择零到
`maximum` 次投影迭代；正整数字面量 `maximum` 同时约束编译后的字段投影与运行时可接受
计数。这不是通用 loop 或计数表达式。

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

## DSL 0.1 最小子集

首个可执行子集使用以下语法。token 之间可以有空白，以及 `//` 或 `/* ... */`
注释。标识符使用 ASCII 字母、数字和 `_`，但不能以数字开头。整数是经过检查的
无符号十进制或 `0x` 十六进制数。字符串支持 `"`、`\\`、`\n`、`\r` 和 `\t` 转义。

```text
program       := { declaration }
declaration   := pure_function | { annotation } ( enum | struct | sequence | entry )
pure_function := "pure" scalar_type identifier "("
                 [ parameter { "," parameter } ] ")"
                 "{" "return" expression ";" "}"
parameter     := scalar_type identifier
enum          := "enum" identifier "{" { enum_member } "}" [ ";" ]
enum_member   := identifier "=" integer ";"
struct        := "struct" identifier "{" { struct_item } "}" [ ";" ]
struct_item   := field | computed | lazy_region | conditional | switch | repeat
field         := { annotation } field_type identifier [ "[" integer "]" ]
                 { annotation } ";"
field_type    := "bits" "<" integer [ "," identifier ] ">" | "ue" | "se"
computed      := { annotation } "computed" "<" scalar_type ">" identifier
                 "=" expression { annotation } ";"
lazy_region   := "@" "lazy" "(" expression ")" "bytes" identifier
                 { presentation_annotation } ";"
presentation_annotation := "@" "description" "(" string ")"
                         | "@" "spec" "(" string "," string ")"
scalar_type   := "bool" | "u64"
conditional   := "if" "(" ( identifier "==" integer | identifier ) ")"
                 "{" { struct_item } "}"
                 [ "else" "{" { struct_item } "}" ]
switch        := "switch" "(" identifier ")" "{"
                 switch_case { switch_case } [ switch_default ] "}"
switch_case   := "case" integer ":" "{" { struct_item } "}"
switch_default := "default" ":" "{" { struct_item } "}"
repeat        := "repeat" "(" identifier "," integer ")"
                 "{" { struct_item } "}"
sequence      := "sequence" "<" identifier ">" identifier "="
                 "scan" "(" identifier ")" ";"
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
- 结构、sequence、enum 和纯函数名共用顶层声明命名空间。语法字段、计算字段与 lazy byte
  region 的名称在结构内统一保持唯一；结构至少包含其中一个 item。
- enum member 名在所属 enum 内不能重复。不同 member 可以使用相同整数值；这些别名接受
  同一个解码数值。
- 纯函数声明 `bool` 或 `u64` 返回类型、最多 16 个名称互异的 `bool` 或 `u64` 参数，以及
  唯一一条 `return` 表达式。函数只能引用自己的参数和此前声明的纯函数。函数 overload、
  forward call、直接或间接递归，以及纯函数上的 annotation 都会被拒绝。每个函数体即使
  没有被使用，也会独立完成类型检查和表达式边界检查。
- `bits<N>` 的宽度必须是 `1..64` 的整数。字段按声明顺序以 MSB-first 消耗输入。
  省略第二个类型参数或写成 `big` 时，得到大端无符号值。`little` 只允许宽度为 8 的倍数、
  字段在结构内从字节边界开始，并且执行时字段的绝对逻辑起点与首个解析出的 source bit 都
  从字节边界开始；后续 mapping segment 的 source 边界不要求按字节对齐。它只反转完整逻辑
  字节的数值权重，不改变实际消耗的 bit 顺序。
- `ue` 和 `se` 是变长 Exp-Golomb 字段，不接受宽度或 endian 参数，并始终按
  MSB-first 消耗编码码字。由于宽度不能静态确定，变长字段之后的小端字段会被拒绝，除非
  后续语言功能能够证明其对齐。
- scalar 字段可以带一个 `[count]` 固定数组后缀。`count` 必须是大于零的无符号整数字面量；
  不接受表达式、额外维度、结构数组或运行时长度。重复名称检查仍使用声明的基础字段名。
  一个结构展开后最多投影 99,999 个 scalar 字段，从而在默认 100,000 个物化节点预算内
  为结构节点保留一个名额；超过该上限时 compiler 不生成可执行 typed IR。
- 固定宽度数组按 `width * count` bit 参与静态对齐。小端数组的每个元素宽度必须是 8 的
  倍数，并且首元素必须从结构内的字节边界开始。`ue` 或 `se` 数组的总宽度未知，因此其
  后续小端字段与单个 Exp-Golomb 字段之后的小端字段一样会被拒绝。
- 等值条件 controller 必须是此前声明、且在到达该条件的每条路径上都保证已经物化的
  scalar `bits`、enum 或 `computed<u64>` 字段。数组、`ue`、`se`、`computed<bool>`、
  未来或未知字段，以及离开所属保证分支后使用的 branch-local 字段都会被拒绝。整数字面量
  必须能由 source field 的无符号宽度表示；`computed<u64>` 接受完整 `u64` 字面量范围。
  简写 `if (flag)` 只接受此前且路径上保证存在的 `computed<bool>`，含义是与 `true` 相等。
  条件可以嵌套，两个分支都会执行静态检查，即使运行时只执行一个。当前切片不接受一般
  条件表达式、布尔组合、`else if` 或其他比较形式。
- 字段名在结构的全部分支中仍必须唯一，所有可能的 branch field 都计入 99,999 字段投影
  上限。静态对齐沿两个分支分别跟踪；只有两条路径都在相同的已知 offset 结束时，条件出口
  才保留已知 offset，省略的 `else` 按空路径处理。
- switch controller 遵守等值条件的声明顺序与路径可用性规则，接受 scalar `bits`、enum
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
  computed controller 则接受完整 `u64` 范围。不接受计数表达式、sentinel/EOF 终止或
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
- 计算字段声明 `computed<bool>` 或 `computed<u64>` 和一条表达式。它可以引用此前声明、
  且在到达当前声明的每条路径上都保证存在的 scalar 无符号 `bits`、enum、`ue` 或计算字段。
  数组、`se`、未知或未来字段，以及路径上不可用的 branch-local 值都会被拒绝。计算字段
  消耗零 bit，不改变静态对齐，继承外层 guard，计入 99,999 字段投影上限，并按与语法字段
  相同的 scope 规则供后续声明使用。repeat 投影会给它的物化名称追加同样的 index。
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
  匹配。无符号 overflow/underflow、除零和模零会在计算字段或 lazy region path 上产生
  runtime `invalid-syntax`。enum 字段提供解码后的 `u64`；enum member 名不是本切片的
  expression value。
- 每个写出的纯函数体、计算字段 expression 或 lazy byte-count expression，以及对应的完全
  内联 expression，深度最多 64，节点最多 256。展开一个函数体或 expression 时，call、
  argument 与参数替换共享最多 4,096
  个 work step；即使 callee
  没有使用某个参数，该参数仍计入上限。pure call 在编译期展开，不引入 runtime call、递归、
  source/host 访问、时间、随机数或可变状态。
- 唯一接受的渐进 sequence 形式是
  `@index(progressive) sequence<Element> name = scan(h264_start_code);`。
  `Element` 必须是已声明结构。
- `@equals(integer)` 字段注解是会执行检查的约束，在一个 `bits` 字段上最多出现一次，且
  参数值必须能由该字段的无符号 bit 宽度表示；`@enum(Type)` 只能出现在 `bits` 字段上，
  最多出现一次，参数必须是已声明 enum 类型，并且它的每个 member 值都必须能由字段宽度
  表示。enum 字段仍解码为无符号整数，enum 为该数值提供名称和有效值检查。`ue` 和 `se`
  拒绝这两个注解。`@description("text")` 提供项目编写的
  展示说明，`@spec("standard", "clause")` 提供规范引用。字段默认继承所属结构的规范
  引用，也可以用自己的注解覆盖。数组声明解析出的类型、注解、metadata 和约束会分别
  应用到每个展开元素。计算字段可在声明前或表达式后使用 `@description` 和 `@spec`，但
  拒绝 `@equals`、`@enum` 和数组后缀。lazy byte region 只在 name 之后接受这两个展示
  annotation。
- 出现词法或静态诊断时，source 不会生成可执行规则；parser 仍返回部分 IR 以及带行列范围的全部诊断，便于编辑器一次报告多个错误。

`enum`、`big`、`little`、`ue`、`se`、`pure`、`return`、`bool`、`u64`、
`computed`、`lazy`、`bytes`、`true`、`false`、`if`、`else`、`switch`、`case`、
`default` 和 `repeat` 只在上述语法位置作为上下文关键字，其他位置仍可作为普通
identifier。既有 scalar 声明保持不变，`bits<N>` 仍与
`bits<N, big>` 完全等价；本切片不弃用任何已接受的 0.1 语法。

parser 生成面向 source、用于诊断的声明模型。静态 compiler 把纯函数、enum、结构、
sequence 和 entry 引用解析成 typed program，保留声明顺序，并确定性生成
`begin-structure`、`read-unsigned-bits`、`read-unsigned-exp-golomb`、
`read-signed-exp-golomb`、`evaluate-computed`、`register-lazy-bytes`、
`assert-equals`、`assert-repeat-count` 和 `end-structure` bytecode。每个 field opcode 必须与
字段类型匹配；
Exp-Golomb typed field 的静态 bit width 为零、使用默认 bit order，且没有 enum reference 或
equality constraint。固定数组按 source 顺序展开成名为 `name[0]` 到
`name[count - 1]` 的 typed field；每个元素各自产生 read instruction，并在存在 `@equals`
时各自产生 assertion instruction。条件 block 被降低到同一条按声明顺序排列的字段流，每个
可能字段携带引用此前 typed-field index 的已解析 presence guard；不会新增 jump opcode 或
一般控制流 bytecode。switch case 字段携带一个正向等值 guard；default 字段携带全部 case
guard 的否定合取，省略 default 时不会为未匹配路径生成字段。嵌套 switch 与条件的 guard
按外层到内层追加。

repeat body 按 `maximum` 次投影到同一条 typed-field 流。第 `i` 次迭代中的每个字段在外层
guard 之后追加一个正向 `count > i` guard；物化名称先追加当前各层 repeat index，再追加
固定数组 index。每个投影后的 repeat 语句记录 controller、maximum、首个 body 字段、外层
guard 和 source range，并在语句位置、首个 body read 之前生成一条带 guard 的
`assert-repeat-count`。这种 lowering 只新增 greater-than presence 比较和边界断言，不新增
jump、回边、可变 index 或替代 source-coordinate 操作。无符号 Exp-Golomb 值会保留下来
作为 repeat controller，但不会因此成为等值条件或 switch 的合法 controller。parser 或
compiler 出现任何诊断时都不生成可执行 typed IR。`svtool rule check` 会运行这两个阶段；
内置 Annex B runner 也只在 analyzer 创建时编译一次规则，之后按已解析的结构索引执行每条
记录。

compiler 会独立 type-check 每个纯函数，再把每个 pure call 展开到使用它的计算字段中。
得到的 typed expression 只包含字面量、此前 typed-field index，以及一元或二元 operator；
不存在 call opcode。计算字段加入同一条 typed-field 流，保留声明类型和 metadata，并继承
已解析的外层 guard。它生成一条 `evaluate-computed` instruction，不推进静态 source offset，
并与语法字段一样获得 repeat index 物化名和 repeat-local scope。任何写出的表达式或展开后
的计算字段表达式超过固定节点、深度或 4,096-step expansion-work 上限时，都不会生成可执行
typed IR。

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
诊断。`@equals` 不匹配时保留该字段，再用 invalid-syntax 诊断标记结构。每个展开的数组元素
都是独立 syntax-field 节点，source location 只覆盖该元素；失败时保留之前完成的元素，不为
未完成元素创建节点，并把 `Header.values[2]` 这样的展开路径写入诊断。mapped backing 的后续
span 读取失败时，reader 保持在字段起点，也不创建半成品字段节点；诊断通过同一 mapping
解析已有逻辑范围，只包含 forwarded source spans。执行器把字段类型、说明和规范引用保留在
analysis-node snapshot 上；展示宽度由节点的逻辑范围推导。

VM 在每次字段读取前按外层到内层的顺序验证并计算 presence guard。guard 为 false 时跳过
该字段，不消耗 source bit、不创建 analysis node，也不执行 enum member 或 `@equals` 数值
检查。选中字段沿用既有数值、诊断和 source-location 行为，因此后续选中字段紧接在上一个
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
location，也不执行 enum member 或 `@equals` 数值检查。选中字段沿用既有数值、metadata、
部分结果和 source 坐标行为，诊断路径包括 `Header.value[1][0]` 这样的投影名称。非法
repeat metadata、controller guard 或 assertion 位置属于 invalid typed definition，运行时
不会猜测执行。

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

direct header 成功后，普通非空 payload 会在不复制字节的情况下从 EBSP 映射为 RBSP logical
view。每个完整的 `00 00 03` 都会排除其中的 `03`，并把它呈现为
`emulation_prevention_three_byte[index]`；相邻的 forwarded byte 会合并为 source span。NAL
子节点固定按以下顺序出现：`start_code`、`NalUnitHeader`、可选的 `rbsp_payload`、按 source
顺序排列的零个或多个 `emulation_prevention_three_byte[index]` region，以及可选的
`trailing_zero_8bits`。NAL unit type `14`、`20`、`21` 需要当前 profile 尚未解析的 extension
header，因此 direct header 之后的字节仍保持 uninterpreted，也不会传给 mapper。

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
但不阻止整体扫描完成。header 读取失败时保留已发布节点，把 root 标记为 invalid，并返回
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

最小非法示例包括 `bits<0> flag;`、`bits<65> flag;`、`bits<12, little> value;`、
位于未对齐字段之后的小端字段、`ue value @equals(0);`、`se value @enum(Type);`、
变长字段之后的小端字段、`bits<1> flags[0];`、`bits<1> flags[];`、数组长度表达式或第二
维、展开后超过 99,999 字段的结构、截断的数组元素、截断的 Exp-Golomb 码字、64 个前导零、`@enum(Missing)`、
无法放入字段宽度的 enum member 值、重复 enum member 名、缺少 `@index(progressive)` 的
sequence、在 `future` 声明前使用 `if (future == 1)`、以数组或 `ue`/`se` 作为条件
controller、条件整数超出 controller 宽度、离开分支后使用 branch-local controller、
`if (flag = 1)`、使用未来字段、数组或 `ue`/`se` controller 的 switch、超出宽度或重复的
case 值、没有 case 的 switch、重复或不位于最后的 default、缺少冒号或花括号 body 的 case、
`break`、fallthrough、同一 arm 的多个 label、case 范围或 enum member label、
使用未知、未来、数组、`se` 或路径上不可用的 branch-local controller 的 repeat、
`repeat (count, 0)`、不能由定宽 controller 表示的 maximum、计数或 maximum 表达式、空
repeat body、repeat 前的 annotation、投影后超过 99,999 字段、在其他迭代或 repeat 之后
使用 repeat-local controller、repeat 之后的小端字段、`scan(other_scanner)`、重复声明同名，
以及没有 `entry` 的程序。

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

## 位置感知上下文目录

runtime infrastructure 为供后续语法引用的 completed definition 暴露一个与具体格式
无关的 context-directory 类型。当前接受的 kind 包含 H.264 SPS/PPS、AAC
AudioSpecificConfig 和 ISO BMFF sample description。key 还包含数字 scope，防止不同
track 中相同 sample-description 或 parameter-set value 冲突。本切片新增 core type，
不新增 session ownership 或 DSL syntax；typed format declaration、payload access
与 runner integration 随官方格式规则实现。

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

目录只持有 key、span、analysis-node 和 dependency identity，typed format payload 仍由
rule owner 保存。目录不读取 source，并遵循 analysis-worker 单写者模型。详见
[ADR-0028](../adr/0028-resolve-context-generations-by-source-position.md)。

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

数组语法不另占运行时预算。每个展开元素消耗一个物化节点和一条 read instruction；每个
`@equals` 元素再增加一条 assertion instruction。因此截断、约束失败、指令上限或节点上限
都可能发生在元素之间，同时保留失败前已经完成的元素。静态的 99,999 字段投影上限确保
一次默认结构物化不会需要超过文档规定的 100,000 个节点。

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
