# 解码场图像 slice header

Status: Accepted
Date: 2026-08-09

## 背景

ADR-0049 确立了一个此后每个 slice-header 增量都沿用的写法：把动态位宽除以一个
必须为 1 的 flag，从而在字段读取源数据**之前**把不成立的布局前提转成
`invalid-syntax`。两个 slice 结构至今仍以这样的字段开头：

```
bits<(context_value(pic_parameter_set_id,
                    h264_sps,
                    log2_max_frame_num_minus4) + 4) /
     context_value(pic_parameter_set_id, h264_sps, frame_mbs_only_flag)> frame_num
```

那个除数并不属于字段宽度。Clause 7.4.3 规定 `frame_num` 恰好占
`log2_max_frame_num_minus4 + 4` bit，无论图像是帧编码还是场编码；这个除法只是
为了拒绝 `frame_mbs_only_flag == 0`。它是 bundled profile 与 Baseline/Main/High
slice header 之间最后一道拒绝，而且波及面异常大——不管后续 header 内容如何，
它都会在第一个 slice header 处拒掉每一条隔行码流和每一条 MBAFF 码流。

Clause 7.3.3 紧接 `frame_num` 之后读取：

```
if (!frame_mbs_only_flag) {
    field_pic_flag
    if (field_pic_flag)
        bottom_field_flag
}
```

同一 clause 也让 `delta_pic_order_cnt_bottom` 的条件变成复合条件：该字段在
imported 的 `bottom_field_pic_order_in_frame_present_flag` 为 1 **且**
`field_pic_flag` 为 0 时存在。场图像不带 bottom-field delta，因为它本身就是一个场。

两条语言限制在这个条件上相遇。对 imported leaf 的 `if` 必须是
`if (context_value(a, b, c) == <integer>)` 这一确切形式，因此复合条件根本无法写成
条件；而 `field_pic_flag` 自身是条件性的，从无条件位置命名它会触发
branch-guarantee 错误。把两者折进一个 `computed<bool>` 解决第一条限制，
ADR-0066 的 `optional_value` 解决第二条。

## 决策

在两个 slice 结构中移除 `frame_num` 上的 `/ frame_mbs_only_flag` 除数，并紧随其后
解码场图像字段：

```
    if (context_value(pic_parameter_set_id,
                      h264_sps,
                      frame_mbs_only_flag) == 0) {
        bits<1> field_pic_flag;
        if (field_pic_flag == 1) {
            bits<1> bottom_field_flag;
        }
    }
```

把 `delta_pic_order_cnt_bottom` 上的 imported-equality guard 换成携带该 clause
完整条件的 computed guard：

```
    computed<bool> has_delta_pic_order_cnt_bottom =
        context_value(pic_parameter_set_id,
                      h264_pps,
                      bottom_field_pic_order_in_frame_present_flag) == 1 &&
        optional_value(field_pic_flag, 0) == 0;
    if (has_delta_pic_order_cnt_bottom) {
        se delta_pic_order_cnt_bottom;
    }
```

`optional_value` 的 fallback 取 `0`，这正是 clause 7.4.3 在 `field_pic_flag` 缺席时
推断的值：`frame_mbs_only_flag == 1` 的码流只编码帧。因此这个 fallback 是在重述
规范的推断，而不是挑一个方便的默认值；渐进式码流的行为与本增量之前完全一致。

有三个不确定点由 probe 而非推理定论。imported-equality 条件接受 `== 0`，而不只是
`== 1`。`bits<1>` 字段可以嵌在 imported-equality 块里，并且其中还能再嵌一层 `if`。
`computed<bool>` 可以用 `&&` 组合一个 imported leaf 与一个 `optional_value`——
ADR-0066 只豁免第一个实参的 branch-guarantee 规则，而这个表达式恰好依赖该豁免。

`pic_order_cnt_lsb` 保留它的 `/ (1 - pic_order_cnt_type)` 除数。POC type 1 与 2
仍在范围之外，那条 ADR-0049 guard 与本条相互独立。本增量移除的是 ADR-0049 所列
五条前提中的一条，而不是这个写法本身。

接受 `frame_mbs_only_flag == 0` 同时也纳入了 MBAFF 帧，而非只有场图像，因为 MBAFF
码流把该 flag 置 0 之后编码 `field_pic_flag == 0`。这是有意的，且不需要额外语法：
两种情况的 slice-header 布局完全相同，宏块自适应帧场编码只改变不透明
`slice_data` 的解释方式，而分析器在当前深度并不解码它。把 MBAFF 单独列为延期项
会错误描述这条规则，因为它的 header 现在能解码了。

## 影响

`rule.toml` 升到 `0.1.21`，并声明覆盖深度 `field-picture-slice-header`。渐进式
码流的解码输出同样会变化，因为 computed field 总会物化成可见的树节点。

34 个分析器测试失败。这是把改动打进规则跑完整套件**实测**得出的，不是估算。全部 34
个都是同一个 +1 child 平移，**没有一个是语义变更**——本增量只增加此前会被直接拒绝的
语法，因此没有任何既有的「某码流应如何解码」期望变成错的。其中 30 个断言了字面 child
count，11 个有序名字列表新增了 `has_delta_pic_order_cnt_bottom` 一项。

两种失败形态的安全性并不对等，值得记录。断言有序名字列表的测试以可读的名字不匹配
失败；而按位置索引 children 的测试**直接 SIGSEGV 崩溃**：`at(8)` 仍然返回一个有效
节点，但那已经是 computed guard，它的 `location()` 为空，于是随后的
`->location()->sourceSpans()` 解引用了一个空 optional。位置索引在这里不只是会过期，
而是**静默**过期，然后在无关的地方炸掉。这正是 ADR-0067「断言完整有序名字列表」这条
约定的具体论据。

插入位置是对两种已解码码流各跑一次 `svtool analyze` **实测**得到的，而不是靠读规则
推导：渐进式 non-IDR slice 为索引 8，IDR slice 为索引 7，差异来自 `idr_pic_id` 与
non-IDR 独有的三个 `computed<bool>` slice-type 字段。

新增 5 个测试：

- `decodesTheBottomFieldPictureNonIdrSliceHeader`——底场，且其 PPS 置起
  `bottom_field_pic_order_in_frame_present_flag`，因此证明的是场图像**抑制**了
  `delta_pic_order_cnt_bottom`，而不只是恰好没有。
- `decodesTheMbaffFrameNonIdrSliceHeaderWithBottomFieldPictureOrderDelta`——MBAFF
  帧，`field_pic_flag == 0` 且 delta 存在，覆盖同一 guard 的另一侧。
- `decodesTheTopFieldPictureIdrSliceHeader`——IDR 结构，两个场标志位落在
  `idr_pic_id` 之前。
- `reportsTruncationBetweenFieldPictureFlagAndBottomFieldFlag`——12 bit 的
  `frame_num` 把 `field_pic_flag` 顶到 payload 的最后一个 bit，于是
  `bottom_field_flag` 无源可读；部分前缀仍然物化，诊断锚定在未读到的字段上。
- `omitsFieldPictureFlagsForAProgressiveSequence`——渐进式回归：两个场标志位都不
  物化，`optional_value` 的 fallback 生效。

码流由生成脚本装配，该脚本先把两个**已提交**的既有 fixture 逐字节复现出来做自检，
因此生成器的 bug 会在那里暴露，而不是变成某个测试里手工调出来的期望值。

分析器套件最终 102 passing；`dev`、`ci`、`sanitize` 三套各 32/32。

## 非目标

场对与互补场对的语义在当前深度不属于分析器范围：本增量解码并呈现
`field_pic_flag` 与 `bottom_field_flag`，不推导 picture order count、不把连续的场
配成对、也不校验码流的场是否交替出现。MBAFF 宏块布局仍留在不透明的 `slice_data`
内部。POC type 1 与 2 仍在 `pic_order_cnt_lsb` 处被拒绝。

`num_ref_idx_l0_active_minus1` 保留 `@range(0, 31)` 界限，不收紧到帧图像的 15。
Clause 7.4.3 规定帧图像上限为 15、场图像为 31，但那是对固定宽度字段取值的语义
约束，而非布局前提——两种情况下字段读取的 bit 数相同，违反它也不会让 header
失步。按图像类型拆分该界限，留给已延期到 clause 7.4.3.3 的 marking 与参考语义
校验。

SP/SI slice 类型仍然延期，且仍然仅属于 Extended profile，因此不属于本增量所推进的
Baseline/Main/High slice-header 里程碑。
