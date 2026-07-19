# Dual Coordinates And Source Mappings

Every parsed field will carry its logical coordinates and an ordered mapping to one or more absolute source spans. Logical views may transform a parent view while preserving mappings through every layer, allowing an H.264 field that crosses a removed emulation-prevention byte to remain logically contiguous and still highlight the correct disjoint file bits. Removed framing or escape bytes remain visible and explicitly classified in the raw data view rather than being silently assigned to a syntax field.

**Consequences**

- A syntax field location is a list of source spans, not a single offset and length.
- Selection, diagnostics, bookmarks, and export must preserve view identity and resolve back to source coordinates.
- Transformations that cannot preserve an exact source mapping cannot produce ordinary bit-addressable syntax fields.
