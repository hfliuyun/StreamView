# ADR-0099: Official MP4 Rule Package, Top-Level Box Traversal, and Container Activation

- **Status**: Proposed
- **Date**: 2026-08-18
- **Authors**: StreamView Contributors

---

## Context

Task P5e establishes the official MP4/ISOBMFF rule package (`org.streamview.mp4`, version `0.1.0`), provides the formal top-level box traversal DSL grammar in `src/mp4_isobmff.svfmt`, embeds the package into the application build, and activates end-to-end MP4 analysis within `Mp4IsobmffAnalyzer`, `AnalysisSession`, and `svtool analyze`.

Following ADR-0096 and ADR-0097:
1. Prior to P5e, `Mp4IsobmffAnalyzer::create(const core::RandomAccessSource&, QString*)` cleanly returned `std::nullopt` because no official rule package was installed in the catalog.
2. In P5e, the bundled rule package `org.streamview.mp4` is embedded via Qt resources and registered, enabling `Mp4IsobmffAnalyzer::create` to resolve the default `video.mp4` entrypoint and analyze valid MP4 sources.
3. The rule package covers:
   - 32-bit standard box size (`size >= 8`);
   - 64-bit `largesize` (`size == 1`);
   - `size == 0` extending to EOF via `available_bytes()`;
   - File Type Box (`ftyp`): `major_brand`, `minor_version`, and repeated `compatible_brands`;
   - Media Data Box (`mdat`): `@lazy` payload;
   - Unknown and opaque boxes: `@lazy` payload without errors;
   - Fragmented MP4 (`moof`): `unsupported` warning at `type` per ADR-0097 D7.
4. All FourCC matching and box format semantics remain strictly within the DSL rule; no FourCC string literals or constants exist in C++ core, runner, scanner, or detector logic.

---

## Decision

### 1. Official Package Manifest (`src/rules/official/org.streamview.mp4/rule.toml`)

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

### 2. Formal MP4 Format Grammar (`src/rules/official/org.streamview.mp4/src/mp4_isobmff.svfmt`)

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

### 3. CMake Resource Integration (`src/rules/CMakeLists.txt`)

Embed the official package into `streamview_rules` library target via `qt_add_resources`:

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

### 4. Runner Activation and Package Loading (`src/rules/mp4_isobmff_analyzer.cpp`)

Implement `loadMp4IsobmffRulePackage()` and `bundledMp4IsobmffRule()` to load the embedded package from `:/streamview/rules/org.streamview.mp4/`, register it with a local catalog, and resolve the `video.mp4` format entrypoint.

When `Mp4IsobmffAnalyzer::create(const core::RandomAccessSource&, QString*, ...)` is called without an explicit rule, it uses `bundledMp4IsobmffRule().resolved` to create the analyzer.

### 5. `svtool` Command-Line Tool Support (`tools/svtool/main.cpp`)

Extend `svtool analyze <source>` to run `detectMp4Candidate`. If confidence is `Strong` and no stronger conflicting formats exist, instantiate `Mp4IsobmffAnalyzer` and analyze the stream.

---

## Consequences

### Positive
- End-to-end MP4 analysis is fully active in `AnalysisSession`, `Mp4IsobmffAnalyzer`, and `svtool analyze`.
- Full compliance with ISO/IEC 14496-12 standard box framing, `ftyp` brand arrays, `mdat` lazy encapsulation, and opaque unknown boxes.
- Strict isolation of format semantics to DSL rules without hardcoding FourCC in C++ core.

### Negative
- Deep container parsing (`moov`, `trak`, `mdia`, `minf`, `stbl`) and sample table windowing remain deferred to P5f/P5g.

---

## References

- ADR-0096: MP4/ISOBMFF Container Architecture, Box Traversal, Cross-Layer Navigation, and Sample Indexing Boundaries.
- ADR-0097: MP4/ISOBMFF Container Primitive Expressibility and Language Increments.
- ISO/IEC 14496-12:2015 Information technology — Coding of audio-visual objects — Part 12: ISO base media file format.
