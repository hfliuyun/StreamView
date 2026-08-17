# ADR-0099: 官方 MP4 规则包、顶层 Box 遍历与容器激活

- **状态**: Proposed
- **日期**: 2026-08-18
- **作者**: StreamView Contributors

---

## 背景

任务 P5e 建立官方 MP4/ISOBMFF 规则包（`org.streamview.mp4`，版本 `0.1.0`），在 `src/mp4_isobmff.svfmt` 中提供正式的顶层 Box 遍历 DSL 语法，将规则包嵌入应用程序构建中，并在 `Mp4IsobmffAnalyzer`、`AnalysisSession` 与 `svtool analyze` 中激活端到端 MP4 分析。

依照 ADR-0096 与 ADR-0097：
1. 在 P5e 之前，`Mp4IsobmffAnalyzer::create(const core::RandomAccessSource&, QString*)` 干净返回 `std::nullopt`，因为目录中尚未安装官方规则包。
2. 在 P5e 中，通过 Qt 资源嵌入并注册内置规则包 `org.streamview.mp4`，使 `Mp4IsobmffAnalyzer::create` 能够解析默认 `video.mp4` 入口点并分析有效 MP4 源。
3. 规则包覆盖：
   - 32 位标准 box size（`size >= 8`）；
   - 64 位 `largesize`（`size == 1`）；
   - `size == 0` 通过 `available_bytes()` 延伸至 EOF；
   - 文件类型 Box（`ftyp`）：`major_brand`、`minor_version` 与重复的 `compatible_brands`；
   - 媒体数据 Box（`mdat`）：`@lazy` 载荷；
   - 未知与不透明 box：`@lazy` 载荷且不报错；
   - 分片 MP4（`moof`）：依照 ADR-0097 D7 在 `type` 处上报 `unsupported` 警告。
4. 所有 FourCC 匹配与 box 格式语义严格保留在 DSL 规则中；C++ 核心、执行器、扫描器或探测器逻辑中不存在任何 FourCC 字符串字面量或常量。

---

## 决策

### 1. 官方包清单（`src/rules/official/org.streamview.mp4/rule.toml`）

```toml
manifest-version = 1

[package]
id = "org.streamview.mp4"
version = "0.1.0"
authors = ["StreamView contributors"]
license = "MIT"
dependencies = []

[compatibility]
language = "0.1"
engine = ">=0.1.0 <0.2.0"

[[entrypoints]]
id = "main"
format = "video.mp4"
source = "src/mp4_isobmff.svfmt"
profiles = ["isobmff"]
depth = "boxes"
detector = "mp4-box"
```

### 2. 正式 MP4 格式语法（`src/rules/official/org.streamview.mp4/src/mp4_isobmff.svfmt`）

```svfmt
@spec("ISO/IEC 14496-12:2015", "4.2")
@description("Top-level ISO Base Media File Format box.")
struct Box {
    bits<32> size
        @description("Box size in bytes. 1 indicates 64-bit largesize; 0 indicates extends to EOF.");
    bits<32> type
        @description("Box FourCC type identifier.");
    if (type == 0x66747970) {
        if (size == 1) {
            bits<64> ftyp_largesize
                @description("64-bit box size in bytes.");
            bits<32> ftyp_large_major_brand
                @description("Major brand identifier.");
            bits<32> ftyp_large_minor_version
                @description("Minor version of major brand.");
            computed<u64> ftyp_large_brand_count = (ftyp_largesize - 24) / 4;
            repeat (ftyp_large_brand_count, 64) {
                bits<32> ftyp_large_compatible_brands
                    @description("Compatible brand identifier.");
            }
        } else {
            if (size == 0) {
                bits<32> ftyp_eof_major_brand
                    @description("Major brand identifier.");
                bits<32> ftyp_eof_minor_version
                    @description("Minor version of major brand.");
                computed<u64> ftyp_eof_brand_count = available_bytes() / 4;
                repeat (ftyp_eof_brand_count, 64) {
                    bits<32> ftyp_eof_compatible_brands
                        @description("Compatible brand identifier.");
                }
            } else {
                bits<32> major_brand
                    @description("Major brand identifier.");
                bits<32> minor_version
                    @description("Minor version of major brand.");
                computed<u64> brand_count = (size - 16) / 4;
                repeat (brand_count, 64) {
                    bits<32> compatible_brands
                        @description("Compatible brand identifier.");
                }
            }
        }
    } else {
        if (type == 0x6D6F6F66) {
            unsupported("fragmented MP4 (moof/traf) is outside the v0.1 subset") at type;
        } else {
            if (size == 1) {
                bits<64> largesize
                    @description("64-bit box size in bytes.");
                computed<u64> large_payload_bytes = largesize - 16;
                @lazy(large_payload_bytes) bytes large_payload
                    @description("Box payload data.");
            } else {
                if (size == 0) {
                    computed<u64> eof_payload_bytes = available_bytes();
                    @lazy(eof_payload_bytes) bytes eof_payload
                        @description("Box payload data extending to EOF.");
                } else {
                    computed<u64> payload_bytes = size - 8;
                    @lazy(payload_bytes) bytes payload
                        @description("Box payload data.");
                }
            }
        }
    }
}

@index(progressive) sequence<Box> boxes = scan(mp4_box);
entry boxes;
```

### 3. CMake 资源嵌入（`src/rules/CMakeLists.txt`）

通过 `qt_add_resources` 将官方包编译嵌入至 `streamview_rules` 静态库目标：

```cmake
qt_add_resources(
    streamview_rules
    streamview_official_rules_mp4
    PREFIX "/streamview/rules/org.streamview.mp4"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/official/org.streamview.mp4"
    FILES
        official/org.streamview.mp4/rule.toml
        official/org.streamview.mp4/src/mp4_isobmff.svfmt
)
```

### 4. 执行器激活与规则包加载（`src/rules/mp4_isobmff_analyzer.cpp`）

实现 `loadMp4IsobmffRulePackage()` 与 `bundledMp4IsobmffRule()`，从 `:/streamview/rules/org.streamview.mp4/` 加载内置包，在局部目录中注册，并解析 `video.mp4` 格式入口点。

当调用 `Mp4IsobmffAnalyzer::create(const core::RandomAccessSource&, QString*, ...)` 时，使用 `bundledMp4IsobmffRule().resolved` 自动创建执行器。

### 5. `svtool` 命令行工具支持（`tools/svtool/main.cpp`）

扩展 `svtool analyze <source>` 增加 `detectMp4Candidate` 调用。若候选置信度为 `Strong` 且无更强的冲突格式，则实例化 `Mp4IsobmffAnalyzer` 驱动码流分析。

---

## 影响

### 正向收益
- 在 `AnalysisSession`、`Mp4IsobmffAnalyzer` 与 `svtool analyze` 中完整激活端到端 MP4 容器分析。
- 全面兼容 ISO/IEC 14496-12 标准 box 结构、`ftyp` brand 数组、`mdat` lazy 封装以及未知 box 的容错处理。
- 格式语义严格隔离在 DSL 规则层，C++ 核心代码保持零 FourCC 硬编码。

### 负向代价
- 深层容器解析（`moov`、`trak`、`mdia`、`minf`、`stbl`）与样本表分页仍保留在后续 P5f/P5g 任务中交付。

---

## 参考文献

- ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries.
- ADR-0097: MP4/ISOBMFF Container Primitive Expressibility and Language Increments.
- ISO/IEC 14496-12:2015 Information technology — Coding of audio-visual objects — Part 12: ISO base media file format.
