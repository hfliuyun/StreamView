# 新增有界 P-slice reference-list modification loop

状态：已接受
日期：2026-08-07

## 背景

ADR-0058 为有界 progressive non-reference P-slice header 新增了可选的 list 0
active-reference count override。下一个必需字段是
`ref_pic_list_modification_flag_l0`，但 bundled rule 仍将它约束为零，因此会拒绝
clause 7.3.3 modification list。

对 P slice 而言，flag 为一时会引入一个 post-tested list 0 loop。每个 operation
以 unsigned Exp-Golomb `modification_of_pic_nums_idc` 开始：值 0/1 选择
`abs_diff_pic_num_minus1`，值 2 选择 `long_term_pic_num`，值 3 终止该 list。
其他值不是合法 operation code。

ADR-0047 已为这一精确语法 shape 新增有界 post-tested sentinel repeat。Unsigned
Exp-Golomb 字段可作为 enum 与 sentinel controller；repeat-local computed Boolean 可以
合并 operation code 0/1，同时为共享 operand 保留单一 source-field 名称。该增量不需要
扩展 engine、context 或 expression language。

## 决策

移除 flag 的 `@equals(0)` constraint。flag 为一时，使用现有 64 次迭代的 sentinel
bound 解码 list 0 operation：

```cpp
enum ModificationOfPicNumsIdc {
    subtract_short_term = 0;
    add_short_term = 1;
    long_term = 2;
    end = 3;
}

bits<1> ref_pic_list_modification_flag_l0;
if (ref_pic_list_modification_flag_l0 == 1) {
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
```

可见的 `uses_abs_diff_pic_num[index]` computed field 没有 source location。operation code
为 0/1 时它为 true，因此两个分支可以发布同一个规范名称的
`abs_diff_pic_num_minus1[index]` source field，而不会重复声明名称。operation code 2
则发布 `long_term_pic_num[index]`。终止 code 3 及其 false computed field 仍会物化；
其后不存在 operand。

`ModificationOfPicNumsIdc` 是闭集 enum。operation code 大于 3 时，保留其完整
Exp-Golomb 字段，并在该字段上致命失败，因为后续布局尚未声明。这不是非致命
`@range` constraint。本切片不约束 operand value，因为其语义合法性依赖 bundled
profile 尚未建模的 decoded-picture-buffer state。

bound 计算包含 terminator 在内的已完成 operation。它是 bundled-profile resource
boundary，不表示 clause 7.4.3 定义了 64 个 operation 的 conformance limit。若 64 个
完整 operation 中没有 terminator，现有 sentinel assertion 会在最后一个
`modification_of_pic_nums_idc[63]` 上失败，同时保留有界 decoded prefix。截断在当前
operation code 或已选 operand 上仍保持事务语义。任一局部失败之后，Annex B
analyzer 都会继续扫描后续 NAL unit。

flag 为零时，loop 不发布字段，既有 QP 与 opaque payload boundary 保持不变。
weighted-prediction 与 CABAC PPS assertion 仍紧随可选 loop，然后是
`slice_qp_delta`。type-1 direct-header assertion 继续要求 `nal_ref_idc == 0`，因此仍排除
`dec_ref_pic_marking()`。

package version `0.1.15` 发布这一 additive rule surface。coverage depth 保持
`i-p-slice-header`，因为该增量只扩展现有 P-slice list 0 分支，没有新增其他
slice family。

回归 fixture 覆盖编码 P 值 0/5、flag-zero absence 路径、reference-index override 之后的
terminating list、全部 operation code 及其选中 operand、未知 operation code、截断的
operation/operand 码字、64 个 operation 仍缺少 terminator 的边界、精确 child order 与
source span、QP 与 opaque payload boundary，以及后续 NAL unit 继续扫描。

## 影响

有界 P-slice header 现在可以描述 list 0 short-term subtraction、short-term addition 与
long-term selection operation，同时保持程序的静态投影与有界运行时工作量。presentation
名称保留 repeat index，因此每个已解码 operation 与 operand 都可以单独定位到 source。

闭集 operation enum 会区分 layout uncertainty 与普通 value-domain warning：未知 operation
不能被默认为“无 operand”并从猜测的 boundary 继续解码。

## 非目标

本决策不新增 list 1 modification、B/SP/SI slice type、field picture、MBAFF、dynamic 或
context-derived list bound、effective reference count、decoded-picture-buffer 或 PicNum 校验、
weighted prediction 或 `pred_weight_table()`、CABAC 或 `cabac_init_idc`、nonzero-reference type-1
header、reference-picture marking、adaptive memory-management operation、POC type 1/2、slice
group、partitioned data 或 CAVLC/CABAC slice-data 解码；不修改 context model、VM、compiler 或
opaque payload 语义。
