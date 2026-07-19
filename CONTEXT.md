# StreamView Analysis

This context defines the language used to inspect local media data from its high-level structure down to individual specification-defined bits.

## Language

**Media source**:
A local file supplied for analysis. It may contain a container or a standalone elementary stream.
_Avoid_: Stream, input file

**Container**:
A media structure that organizes one or more tracks and their encoded data together with timing and descriptive information.
_Avoid_: Media format, wrapper

**Elementary stream**:
A sequence of encoded audio or video data governed by a codec syntax, such as H.264 video or AAC audio, without relying on a container for its internal interpretation.
_Avoid_: Raw stream, codec file

**Syntax field**:
A named value defined by an applicable container or codec specification and encoded in one or more mapped spans of the media source.
_Avoid_: Property, metadata

**Bit-level analysis**:
Interpretation of a media source that exposes each parsed syntax field, its value, and the source bits from which it was derived.
_Avoid_: Hex dump, media probing

**Structural syntax**:
The framing, headers, parameter sets, timing information, and other directly navigable syntax fields that describe encoded media without decoding its compressed payload.
_Avoid_: Header-only parsing

**Compressed payload**:
The entropy-coded audio or video data whose fields require a codec's coding state, such as H.264 CAVLC/CABAC data or AAC Huffman spectral data.
_Avoid_: Raw bytes, undecoded data

**Parsing depth**:
The declared boundary of how far a format parser interprets a media source, distinguishing structural syntax from compressed payload analysis.
_Avoid_: Support level, completeness

**Format definition**:
A declarative description of a container or codec's syntax that tells the analyzer how to locate, decode, label, and explain its fields.
_Avoid_: Template, parser script

**Analysis rule**:
A loadable format definition or decoding extension used to interpret a particular media structure.
_Avoid_: Plug-in, profile

**Support tier**:
The level of platform compatibility promised for a release: a primary tier is continuously validated, while a secondary tier shares the same architecture but is not validated for every release and version combination.
_Avoid_: Platform support, compatibility

**Local analysis source**:
A local, randomly accessible file used as the first-release input for structural media analysis, whether it is a container or an elementary stream.
_Avoid_: File stream, offline stream

**Network/live source**:
A URL, capture device, pipe, or other progressively arriving input whose data cannot be assumed to be complete or randomly accessible.
_Avoid_: Real-time file, live file

**Read-only analysis**:
Inspection that may locate, copy, annotate, or export interpreted data but never modifies or writes back to the media source.
_Avoid_: Safe editing, inspection mode

**Media sample**:
An encoded unit referenced by a container track, such as one AVC access unit or one AAC access unit, together with the source range needed to inspect it.
_Avoid_: Packet, frame (when the container has not established frame semantics)

**Cross-layer navigation**:
Movement from a container structure to the media sample it references and then into that sample's codec syntax fields, while preserving the corresponding source locations.
_Avoid_: Drill-down, linked parsing

**Declared format support**:
A tested promise for a named container, codec profile, and parsing depth. Recognizing a format or partially decoding known fields does not by itself establish declared support.
_Avoid_: Opens successfully, best-effort support

**Unsupported syntax**:
Recognized input that falls outside the declared format support and is preserved in the analysis with an explicit limitation instead of being presented as fully parsed.
_Avoid_: Unknown bytes, parse failure

## Workspace

**Analysis tree**:
The hierarchy of container structures, tracks, media samples, codec units, and syntax fields derived from a media source.
_Avoid_: Object tree, parse tree

**Raw data view**:
The byte- and bit-addressable representation of the unchanged media source, displayed in hexadecimal, binary, or combined form.
_Avoid_: Hex editor, file editor

**Field inspector**:
The detailed presentation of a selected syntax field, including its value, width, source location, meaning, specification reference, and diagnostics.
_Avoid_: Properties panel, metadata panel

**Synchronized selection**:
The bidirectional relationship in which selecting a syntax field highlights its exact source bits and selecting source bits locates the most specific containing field in the analysis tree.
_Avoid_: Linked selection, synchronized views

**Lazy analysis**:
Parsing that discovers and materializes a source structure only when needed for the current view or navigation request, while allowing background indexing to improve later access.
_Avoid_: Partial load, streaming parse

**Analysis session**:
The read-only, cancellable state associated with one opened local analysis source, including discovered structures, diagnostics, indexes, and user navigation state.
_Avoid_: Open document, parser instance

**Parse diagnostic**:
An explicit report attached to a source location and field path when the input is malformed, truncated, unsupported, or otherwise requires attention during analysis.
_Avoid_: Parser error, log message

**Partial result**:
The valid portion of an analysis tree retained after a parser encounters a diagnostic, with the unresolved region clearly marked instead of silently guessed.
_Avoid_: Best-effort tree, incomplete parse

**Format definition language**:
The constrained declarative language used to describe syntax fields and compose bounded decoding operations for analysis rules.
_Avoid_: Scripting language, plug-in API

**Rule package**:
A versioned, loadable bundle containing one or more format definitions, their applicability metadata, and compatibility information for the analyzer.
_Avoid_: Script bundle, extension

**Language version**:
The independently versioned contract that defines format-language syntax and runtime semantics, separate from application and rule-package releases.
_Avoid_: Engine version, file-format version

**Compatibility range**:
The explicit set of language and analysis-engine versions under which a rule package declares that it can be loaded and interpreted.
_Avoid_: Minimum version, best-effort compatibility

**Bundled rule**:
An official analysis rule distributed with a specific StreamView application release and available without separate installation.
_Avoid_: Built-in parser, system rule

**Installed rule**:
A rule package added from a local file or directory and executed with the same sandbox permissions as every other rule.
_Avoid_: Third-party plug-in, trusted rule

**Pinned rule version**:
The exact rule-package version retained by a saved session so later installations or upgrades cannot silently change its analysis behavior.
_Avoid_: Selected version, rule lock

**Package manifest**:
The human-readable TOML record that identifies a rule package, its version and license, its language and engine compatibility, its entry points, format coverage, detection metadata, and documentation.
_Avoid_: Configuration file, package metadata

**Development package**:
The editable directory form of a rule package, containing its manifest, format definitions, documentation, and distributable tests.
_Avoid_: Source folder, unpacked package

**Packaged rule**:
The deterministic ZIP-container form of a development package used for local installation, containing no native executable code, symbolic links, or paths outside its package root.
_Avoid_: Plug-in archive, binary package

**Rule sandbox**:
The execution boundary that gives a format definition controlled read-only access to the media source while denying arbitrary host, network, and process access.
_Avoid_: Security wrapper, plug-in sandbox

**Field declaration**:
A C-style statement in a format definition that consumes source data and creates a syntax field with a value and source location.
_Avoid_: Member variable, property declaration

**Computed field**:
A named value derived without consuming source bits, kept distinct from encoded syntax fields in the analysis tree.
_Avoid_: Virtual field, calculated metadata

**Source coordinate**:
An absolute byte-and-bit position in the unchanged media source.
_Avoid_: File position, physical offset

**Logical view**:
A read-only sequence presented to a format definition after a reversible structural transformation, such as removing H.264 emulation-prevention bytes from an EBSP to obtain an RBSP.
_Avoid_: Decoded buffer, temporary stream

**Logical coordinate**:
A byte-and-bit position within a logical view, meaningful only together with that view's identity.
_Avoid_: Virtual offset, parser position

**Source span**:
A contiguous range of bits in the media source identified by source coordinates.
_Avoid_: Byte range, selection

**Source mapping**:
The ordered relationship from a logical range to one or more source spans, preserved through nested views so every parsed bit can be traced back to the media source.
_Avoid_: Offset translation, position map

**Mapped transformation**:
A bounded rule operation that constructs a logical view by forwarding, skipping, or slicing source bits while retaining an exact source mapping for every forwarded bit.
_Avoid_: Decode step, buffer conversion

**Excluded span**:
A source span intentionally omitted from a logical view by a mapped transformation and retained with a named structural role, such as an H.264 emulation-prevention byte.
_Avoid_: Ignored byte, discarded data

**Lazy region**:
A bounded source or logical range whose existence and limits are known but whose internal syntax fields are materialized only when requested.
_Avoid_: Unparsed payload, deferred buffer

**Progressive index**:
A cancellable, resumable sequence of discovered structures published incrementally while an analysis session scans a source.
_Avoid_: Background parse, partial index

**Specification reference**:
Versioned metadata that identifies the standard, clause, table, or named syntax production governing a syntax field, without embedding the standard's normative text.
_Avoid_: Documentation link, help text

**Field explanation**:
A concise, project-authored description of a syntax field's meaning and value interpretation, presented separately from its specification reference.
_Avoid_: Standard text, field comment

**Format detection**:
Bounded inspection of a media source that produces candidate format definitions using structural signatures and reports the evidence and confidence for each candidate.
_Avoid_: File-type guess, extension check

**Rule selection**:
The explicit choice of the format definition actually used for an analysis session, whether suggested by format detection or chosen manually by the user.
_Avoid_: Auto-detect result, parser choice

**Saved session**:
A separate persistent record of an analysis session's source identity, rule selection, bookmarks, annotations, and navigation state that never contains modifications to the media source.
_Avoid_: Project file, edited media file

**Source fingerprint**:
A compact identity derived from source attributes and content that detects whether a saved session is being reopened against different media data.
_Avoid_: File hash, cache key

**Bookmark**:
A user-named reference to a syntax field or source bit range retained in a saved session.
_Avoid_: Marker, favorite
