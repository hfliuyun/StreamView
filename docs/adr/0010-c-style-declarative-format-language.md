# C-Style Declarative Format Language

Format definitions will use a constrained C-style syntax with typed field declarations, structures, enums, composition, conditionals, bounded loops, pure helper functions, computed fields, and specification annotations. The language deliberately excludes C++ object semantics such as inheritance, constructors, templates, pointers, mutation methods, exceptions, and native calls. This keeps rules familiar to C/C++ authors and close to standards pseudocode without turning the rule sandbox into a general-purpose compiler or runtime.

**Considered Options**

- YAML or JSON schemas: easy to parse, but cumbersome for the conditional and state-dependent syntax found in media specifications.
- Full C++-style classes: familiar surface syntax, but object lifetime, inheritance, mutation, and native interoperability would greatly expand the language and security boundary.
