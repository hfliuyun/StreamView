# Rules Declare Lazy Boundaries

Format definitions will explicitly declare bounded lazy regions and progressive indexes. Rules parse only the fields needed to establish a safe region boundary, materialize lazy contents on demand, and publish indexed structures incrementally during cancellable scans. This prevents the engine from guessing format-specific dependencies and connects the rule language directly to the requirement to inspect files of at least 100 GB without building a complete in-memory tree.

**Consequences**

- Every lazy region must have a checked upper bound before it can be registered.
- Dependencies that force additional parsing must be declared and visible to diagnostics and progress reporting.
- Not-yet-materialized, unsupported, invalid, and cancelled regions are distinct analysis states.
