# StreamView Product Requirements

Status: accepted first-milestone baseline, explicitly confirmed after the design interview. English is normative; the maintained [Chinese companion](zh-CN/product-requirements.md) is provided for accessibility.

## Purpose And User

StreamView is a read-only desktop binary analyzer for media developers, protocol investigators, and advanced users. It reveals local media from container structure through tracks and samples into specification-defined codec fields, preserving exact byte-and-bit traceability to the unchanged source.

## First-Milestone Inputs

- Complete, randomly accessible local files only.
- Standalone H.264 Annex B elementary streams.
- Standalone AAC ADTS elementary streams.
- Non-fragmented ISO BMFF MP4/MOV containing AVC and AAC.

Network URLs, RTP/RTSP, HLS/DASH, capture devices, pipes, standard input, and packet captures are deferred.

## Declared Format Support

- H.264 Baseline, Main, and High structural syntax for common 8-bit 4:2:0 content: start codes, NAL units, EBSP/RBSP mapping, SPS, PPS, SEI, and slice headers.
- AAC-LC structural syntax in ADTS and MP4 AudioSpecificConfig.
- Recursive non-fragmented MP4/MOV boxes, tracks, sample tables, `avcC`, and `esds`, with navigation from a container sample into H.264 or AAC syntax.

Fragmented MP4, MPEG-TS, Matroska/WebM, H.264 CAVLC/CABAC macroblock and residual decoding, AAC Huffman spectral payload decoding, and other profiles are deferred. Playback and complete audio/video decoding are not part of the first milestone.

## Analysis Behavior

- Every syntax field exposes its decoded value, type, width, source location, project-authored explanation, and versioned specification reference.
- A field can map to multiple absolute source spans and also has coordinates within its logical view.
- Mapping-preserving transformations expose excluded bytes such as H.264 emulation-prevention bytes instead of hiding them.
- Format detection recommends candidates with evidence and confidence; the user can always select another compatible rule.
- Malformed, truncated, and unsupported syntax produces source-located diagnostics and retains valid partial results without silently guessing values.
- Media sources are strictly read-only. Editing and write-back are excluded.

## Desktop Workspace

- An analysis tree shows containers, tracks, samples, codec units, and fields.
- A virtualized raw data view provides hexadecimal, binary, and combined modes with bit-level highlighting.
- A field inspector shows value, width, absolute and logical coordinates, meaning, specification reference, and diagnostics.
- Selection is bidirectional between fields and their exact source spans.
- Cross-layer navigation moves from MP4 sample metadata into the contained codec syntax and back.

## Large Files

- Inspect files of at least 100 GB without memory use growing linearly with source size.
- Make the initial view useful in approximately two seconds under normal local-storage conditions.
- Materialize bounded regions on demand and build indexes progressively in the background.
- Publish scan results incrementally; long work reports progress and can be cancelled while retaining completed results.
- Jumping to a known offset does not wait for a complete-file scan.

## Sessions And Export

- Saved sessions are separate from the read-only media source and retain source identity, exact rule versions, bookmarks, annotations, and navigation state.
- A source fingerprint detects media changes before saved locations are reused.
- Rebuildable large caches remain outside the compact saved session.
- Hierarchical JSON export is desirable but does not block the first milestone.

## Rule System

- Built-in and installed rules use the same sandboxed C-style declarative language.
- Rules are bounded, deterministic, read-only, cancellable, and denied arbitrary host, network, process, pointer, and native-library access.
- Rules define source-mapped logical views, safe lazy boundaries, and progressive indexes.
- Application, language, and package versions are independent; saved sessions pin exact rule versions.
- Official rules ship with the application. The first release installs additional packages only from local files or directories and has no marketplace or automatic update.
- The detailed language and package contract is maintained in the [format-language reference](format-language/README.md).

## Platforms, Implementation, And Release

- Primary validated platforms: Windows 11 x64, macOS 14+ Apple Silicon, and Ubuntu 24.04 LTS x64.
- Secondary compatibility targets: Windows 10, Intel Macs, and other modern x64 Linux distributions.
- C++20 analysis core, Qt 6 Widgets desktop UI, CMake builds, and Python only for development and testing utilities.
- GitHub Actions builds and tests the primary platform matrix for every change and creates platform-specific artifacts from release tags.
- First releases may be unsigned portable artifacts; Windows signing and macOS notarization are deferred.
- StreamView and official rules use the MIT License. Qt is dynamically linked under its applicable open-source license obligations; GPL-only Qt modules are avoided unless the distribution policy is revisited.

## Documentation

- English technical specifications, ADRs, format-language reference, and API contracts are normative.
- Maintained Chinese documentation covers stable features, onboarding, and concepts; English controls if wording conflicts.
- A language feature is not stable until syntax, semantics, coordinates, diagnostics, limits, compatibility, and valid/invalid examples are documented.
