# ADR-0106: 格式检测仲裁与证据不对称

- **状态**：Proposed
- **日期**：2026-08-30
- **作者**：StreamView Contributors

---

## 上下文

StreamView 对本地分析源的首 64 KiB 执行有界格式检测，据此选定分析会话所用的
格式定义。参与检测的有三个检测器：`detectMp4Candidate`、
`detectAacAdtsCandidate` 与 `detectH264AnnexBCandidate`。每个返回一个可选候选
及其三值信心（`Weak`、`Probable`、`Strong`）。

在这三个结果的仲裁方式中发现了两个缺陷。

### 缺陷 1：模式证据对容器证据拥有一票否决权

检测结果的两个消费方各自独立实现了同一个谓词：

```text
选择 MP4    当 mp4 == Strong && h264 != Strong && aac != Strong
选择 AAC    当 aac == Strong && h264 != Strong
其余情况    H.264
```

该谓词把三个信心视为可通约，并赋予 H.264 的 `Strong` 无条件否决权。但它们并不
可通约：

- MP4 的 `Strong` 是一个**全局结构不变式**。它意味着从偏移 0 起有三个或更多箱子
  连续铺砌，每个箱子声明的长度都精确落在下一个箱子头部。
- H.264 或 AAC 的 `Strong` 是一个**局部字节模式**。Annex B 检测器只需在受检窗口
  内任意位置找到两处有效的起始码加头部即可达到 `Strong`，并不要求第一处位于源的
  起始位置。

AVC-in-MP4 文件必然在其 `mdat` 内携带 H.264 头部，而 `avcC` 记录在 `moov` 内携带
SPS/PPS 字节。两者都会常规性地产生两个以上有效 Annex B 头部。因此该否决权会在
普通、格式良好的输入上触发：真实的 AVC-in-MP4 源被当作裸 H.264 Annex B 基本流
分析，整个容器结构从未得到呈现。

修复前在仓库 fixture 上实测：`mp4_p5j4_avc_multi_nal.mp4` 与
`mp4_p5j4_two_tracks.mp4` 均从偏移 0 铺砌至 EOF，均达到 MP4 `Strong`，却仍被路由
到 Annex B 分析器。

### 缺陷 2：该谓词存在两份

同一谓词在应用会话层与内部 `svtool` CLI 中被各自独立写出。一项检测策略存在两份
拷贝时无法一次性修正，并且可能静默分化，导致两个界面对同一个源给出不一致的判断。

### 约束

ADR-0099 第 24 条要求所有 FourCC 匹配与 box 格式语义严格限于 DSL 规则之内，C++
核心、运行器、扫描器与检测器逻辑中不得出现 FourCC 字符串字面量或常量。因此最直觉
的修法——检查首个箱子类型是否为 `ftyp`——不可用。

## 决策

### 1. 单一仲裁点，置于 rules 层

`src/rules/format_selection.{h,cpp}` 中的 `selectFormatFromDetection` 是唯一决策
点。它消费三个检测结果，返回携带所选 `DetectedFormat` 与 `DetectedFormatReason`
的 `FormatSelection`。分析会话与 `svtool` 均调用它，两者都不再重复实现策略的任何
部分。

### 2. 仲裁仅依据源坐标判定

该函数只检视检测器已产出的证据跨度的几何关系。它不读取任何 box 类型、brand 或
编解码器名称，因此 ADR-0099 第 24 条继续成立；实现中 FourCC 字面量数量为零。

三个派生判据：

- **覆盖终点**——`Strong` MP4 铺砌中最远的已验证箱子终点，钳制到受检字节数。由于
  每个被记录的箱子都从偏移 0 起连续，最远终点即容器所能解释的范围边界。覆盖终点
  仅对 `Strong` 铺砌计算；`Weak` 铺砌常常只是被读成箱子大小的载荷字节模式，若允许
  它宣称字节归属，会压制真实的基本流。
- **包含性**——每一个起始码或同步字是否都位于覆盖终点之下。被包含的证据不携带任何
  容器尚未解释的信息。
- **锚定性**——首个有效起始码或同步字是否始于源起始处。ITU-T H.264 Annex B 允许在
  首个起始码前缀之前存在前导零字节，故 H.264 锚定允许至多三个前导零字节
  （`maximumAnnexBLeadingZeroBytes = 3`）；ADTS 不允许前导零字节，故 AAC 锚定严格
  要求位于偏移 0（`absoluteBitOffset() == 0`）。首次出现在源深处的起始码或同步字是
  字节巧合，而非流的起点。

模式证据仅在**独立于**容器铺砌时才反超之：既锚定于源起始，**又**越过覆盖终点。任一
条件单独都可由巧合满足——锚定可由低位字节读作起始码的箱子大小满足，越界可由任意
载荷字节满足——故两者必须同时成立。

### 3. 不可约碰撞判归容器并显式标记

字节可以同时既以有效起始码或同步字开头，又在整个窗口内铺砌成箱子。任何不含 FourCC 的
规则都无法在两个方向上判定该情形。此时选择判归容器，并报告
`AmbiguousContainerVersusElementaryStream`，它与普通容器胜出的理由相区分。歧义被
记录而非被当作事实呈现，使该决策保持可复核。（要求：`ambiguous()` 必须在 Task P5j-5
中落到用户可见的 UI 呈现面）。

### 4. 两个模式检测器的相对次序不变

两个模式检测器之间互不包含，故 H.264 保留其相对 AAC 的原有优先级。本 ADR 仅修正
容器与模式之间的仲裁。

## 后果

### 正面

- 真实 AVC-in-MP4 源被当作容器分析。已通过对五个 MP4 fixture 执行
  `svtool analyze` 验证，全部物化出 box 树。
- 真实 Annex B 输入不受影响，包括首个起始码前含前导零字节的输入。
- 检测策略在唯一位置上得到修正，对所有界面生效。
- 选择结果携带理由，使歧义决策可与确信决策相区分。

### 负面与推迟项

- **真实大型 MP4 源仅达到 `Probable`。** 形如 `ftyp` + `moov` + 大 `mdat` 的文件
  在受检窗口内只记录到两个箱子，因为 `mdat` 头部声明的长度越出窗口，而检测器会
  丢弃无法完整验证的箱子。此类源达不到 `Strong`，因而无法从本次仲裁中获益。该阻塞
  将在 P5j-5 开工前通过专门的检测器信心判据切片（Task P5j-4e）单独修复。
- **未识别输入仍回退至 H.264 分析器。** 产品需求要求检测未命中时不得拒绝该源，故
  保留此回退。对非 H.264 输入而言其诊断具有误导性；一条正式的未知源路径已单独
  登记。
- 锚定在 H.264 侧采用固定的三字节前导零窗口。该设定仅为接纳带前导零的真实 Annex B 流，
  排除容器依赖的是包含性判据而非此窗口。若一个合规流带有更长的前导零字节序列，将不被
  视为已锚定。

## 验证

- `tests/rules/format_selection_test.cpp` 中 14 条对抗性用例，覆盖真实 fixture、
  真实 Annex B、带前导零字节的 Annex B、4 字节前导零未锚定 Annex B、部分铺砌下的
  未锚定 H.264 与 AAC 模式、首箱大小读作起始码的容器、不可约 H.264 与 AAC 碰撞、
  被包含的模式证据、未达 `Strong` 的候选，以及无结构字节。无任何用例被条件断言
  包裹或被跳过。
- 单变量反向变异验证：
  - 移除 `aacAnchored` 单项导致 `unanchoredAacPatternCannotOutrankPartialContainerTiling` 转红（退回 `AacAdts`）；
  - 移除 `h264Anchored` 单项导致 `unanchoredH264PatternCannotOutrankPartialContainerTiling` 转红（退回 `H264AnnexB`）；
  - 既有 44 条会话测试仍全部通过——证实旧测试集对这些仲裁路径存在结构性失明。
- 直接对五个 MP4 fixture 与两个 Annex B 输入执行 `svtool analyze`。

## 参考

- [ADR-0099：MP4 官方规则包与激活](0099-mp4-official-rule-package-and-activation.md)
- [产品需求](../product-requirements.md)
- ISO/IEC 14496-12，box 结构
- ITU-T H.264 Annex B，字节流格式
