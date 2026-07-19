# Tolerant Partial Analysis

Format definitions will produce partial results and precise parse diagnostics whenever a source is malformed, truncated, or outside declared support. Analysis stops only when the source cannot be read or no safe boundary remains; valid structures and raw locations already discovered remain available. This favors forensic inspection and debuggability over fail-fast behavior, while explicitly distinguishing invalid input from unsupported syntax.
