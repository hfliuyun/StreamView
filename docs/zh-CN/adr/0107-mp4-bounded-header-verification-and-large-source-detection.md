# ADR-0107: MP4 有界头部验证与大源检测

- **状态**：Accepted
- **日期**：2026-08-30
- **作者**：StreamView Contributors

---

## 背景

StreamView 在本地分析源的有界前缀（默认 64 KiB，`mp4DetectionProbeSizeBytes()`）上执行格式检测。在 `src/rules/mp4_box_detector.cpp` 中，`detectMp4Candidate` 自偏移 0 起扫描连续铺砌的 box，将每个经校验的 box 记入 `evidence`，并计算置信度：
- 3 个及以上 box：`Strong`
- 2 个 box：`Probable`
- 1 个 box：`Weak`

在 ADR-0106 中，`src/rules/format_selection.cpp` 的格式仲裁确立了 MP4 候选必须达到 `Strong` 置信度，才能对模式检测器（H.264 Annex B 或 AAC ADTS）主张容器归属。

### 缺陷：窗口边界处的越窗 box 被整体丢弃

在 ISO/IEC 14496-12 媒体文件中，媒体有效载荷 box（`mdat`）通常极为庞大（数十兆至数吉字节）。当扫描典型的 faststart 影片布局（`ftyp` + `moov` + 巨大 `mdat`）时：
1. `ftyp`（通常 24–32 字节）在 64 KiB 窗口内完全验证；
2. `moov`（通常 1–10 KiB）在 64 KiB 窗口内完全验证；
3. `mdat` 起始于 64 KiB 窗口内某一偏移，其 8 字节（或 16 字节 largesize）头部完全位于窗口之内。然而其声明长度（`boxSize`）远超出 64 KiB 探测上限。

在修复前的实现中（`mp4_box_detector.cpp:108-111`）：

```cpp
if (boxSize > remainingInInspection) {
    // Box extends beyond inspection window or is truncated in source
    break;
}
```

扫描器一旦遇到 `boxSize > remainingInInspection` 便无条件 `break`，彻底丢弃了 `mdat`。导致以下后果：
- 检测器仅记录了 2 个 box（`ftyp` 与 `moov`），只能获得 `Probable` 置信度，未能达到 `Strong`；
- 在 `format_selection.cpp` 中，因 `mp4Strong` 要求 `confidence == Strong`，容器仲裁无法宣称容器胜出。由于 AVC-in-MP4 的 `mdat` 内部必然包含有效的 H.264 NAL 头部，`detectH264AnnexBCandidate` 达到 `Strong`。由于 `mp4Strong` 为假，该源错误退回裸流兜底 `H264AnnexB`；
- 在非 faststart 文件（`ftyp` + 巨大 `mdat` + 尾部 `moov`）中，丢弃 `mdat` 导致窗口内仅余 1 个 box（`ftyp`），退化为 `Weak`；
- 在首个 box 即为 64 位 largesize `mdat`（`size == 1`）的文件中，丢弃 `mdat` 导致 MP4 候选完全为空（`nullopt`），而偏移 0 处的 `00 00 00 01 6D ...` 被 Annex B 检测器误判为锚定的 H.264 NAL 头（类型 13）。

### 约束

1. **禁止单纯放宽箱数阈值**：复审裁定（P5j-4d 裁定 3）明确禁止单纯将 `Strong` 阈值下调至 2 个 box。双箱铺砌特异性不足，会在任意短数据上引发误报；
2. **恪守覆盖终点不变量**：ADR-0106 依赖 `boxSpan` 计算 `mp4CoverageEndBytes`。越窗 box 的未检查主体（body）**绝对不得**计入 `boxSpan` 或 `coverageEnd`，否则未经验证的载荷字节将被虚假宣称为结构已覆盖。

## 决策

### 1. 越窗 box 的有界头部验证

当 `detectMp4Candidate` 遇到声明 `boxSize` 超出 `remainingInInspection` 的 box 时：
1. 检查该 box 在源文件中是否截断：
   ```cpp
   const quint64 remainingInSource = sourceSizeBytes - offset;
   if (boxSize > remainingInSource) {
       break; // 源文件截断
   }
   ```
   若声明 box 长度超过源文件剩余实际字节，说明文件截断，该 box 无效。
2. 校验 bit 坐标溢出：
   ```cpp
   if (!checkBitCoordinateOverflow(offset, boxSize)) {
       break;
   }
   ```
3. 验证 box 头部是否完整位于检查窗口内（`remainingInInspection >= headerBytes`，其中 `size == 1` 时 `headerBytes` 为 16，`size >= 8` 时为 8）。该检查在读取 `size` 和 `largesize` 前已满足。
4. 将该 box 记入 `evidence`，且**仅以头部长度建立源区间**：
   ```cpp
   const auto span = core::SourceSpan::create(
       core::SourceBitAddress(offset * 8U), headerBytes * 8U);
   if (!span.has_value()) {
       break;
   }

   Mp4DetectionEvidence ev;
   ev.boxSpan = span;
   ev.boxOffset = offset;
   ev.declaredBoxSize = declaredSize;
   evidence.push_back(ev);
   break;
   ```
5. 终止扫描（`break;`），因为下一个 box 的起始偏移位于 `offset + boxSize`，已超出探测窗口。

### 2. 仅头部区间保护覆盖终点

通过将 `ev.boxSpan` 严格限定为经验证的 `headerBytes`（8 或 16 字节）：
- 所有证据区间的终点均小于或等于 `result.inspectedByteCount`；
- `mp4CoverageEndBytes` 仅覆盖至越窗 box 的经验证头部（如 `ftyp` + `moov` + 8 字节 `mdat` 头）；
- `mdat` 内部未经验证的有效载荷字节均位于 `coverageEnd` 之外。其中的模式证据（如 `mdat` 内的 H.264 起始码）判定为 `!contained`。但由于首个起始码并不位于偏移 0（`!h264Anchored`），`h264Independent` 仍为假；
- faststart 影片（`ftyp` + `moov` + `mdat`）在 `evidence` 中拥有 3 个 box，达成 `Strong`。容器仲裁判定为 `Mp4Isobmff`，原因为 `Mp4SubsumesPatternEvidence`。

## 影响

### 正向影响

- 真实世界的大型 MP4 文件（如 `ftyp` + `moov` + 数吉字节 `mdat`）能够达到 `Strong` 置信度，并被正确识别为 `Mp4Isobmff`；
- 保留了 ADR-0106 的覆盖终点不变量：未检查的字节绝不会被虚假标记为已覆盖；
- 探测窗口内的 64 位 largesize box 头部（`size == 1`）能够被记入 `evidence`；
- 声明长度超过源文件大小的截断 box（`boxSize > sourceSizeBytes - offset`）仍被正确拒绝。

### 负向与延后项

- 非 faststart 文件（`ftyp` + 巨大 `mdat` + 尾部 `moov`）在 64 KiB 探测窗口内仅能记录 2 个 box（`ftyp` 与 `mdat` 头部），置信度为 `Probable`。在存在模式证据时识别此类非 faststart 容器需要双端探测（读取源末尾寻找 `moov`），延后至容器探测专属切片处理。

## 验证

1. `tests/rules/mp4_box_detector_test.cpp`：
   - `acceptsWindowExceedingBoxWhenHeaderIsVerifiable`：普通 box（32768 + 32732 + 偏移 65500 处的 1000 字节 box）达成 3 箱 `Strong`；第 3 个 box 的 span 恰好覆盖 8 字节头部，终点为 65508；
   - `acceptsWindowExceedingLargeBoxWhenHeaderIsVerifiable`：large box（32768 + 32732 + 偏移 65500 处的 1000 字节 large box）达成 3 箱 `Strong`；第 3 个 box 的 span 恰好覆盖 16 字节头部，终点为 65516；
   - `evidenceSpanExceedingProbeWindowIsRejected`：偏移 60000 处声明 10000 字节的第 4 个 box 头部被记为第 4 条证据，其 span 终点为 60008 <= 65536；
   - 真实规模合成 fixture（`ftyp` 32 + `moov` 2000 + 500 MB `mdat`）达成 `Strong` 置信度与 `evidence.size() == 3`；
   - 越出源文件总大小的截断 box（`boxSize > sourceSizeBytes - offset`）被拒绝进入 `evidence`。
2. `tests/rules/format_selection_test.cpp`：
   - 真实规模合成 MP4（`ftyp` + `moov` + 500 MB `mdat`，`mdat` 内含 AVC NAL 起始码）仲裁选择 `Mp4Isobmff`，原因为 `Mp4SubsumesPatternEvidence`。
3. `dev`、`ci`、`sanitize` 三套预设下全部既有测试继续通过。

## 参考

- [ADR-0099: MP4 Official Rule Package and Activation](0099-mp4-official-rule-package-and-activation.md)
- [ADR-0106: Format Detection Arbitration and Evidence Asymmetry](0106-format-detection-arbitration-and-evidence-asymmetry.md)
- ISO/IEC 14496-12, Section 4.2 (Box structure)
