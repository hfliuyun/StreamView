# Sandboxed Declarative Rules

Built-in and user-supplied analysis rules will run as versioned declarative format definitions inside a constrained rule sandbox. Rules may perform bounded, cancellable, read-only source access and registered pure decoding operations, but may not access arbitrary host files, the network, environment variables, processes, or native plug-ins. This preserves the requested 010 Editor-style extensibility without making every rule an unreviewable cross-platform code-execution surface.

**Considered Options**

- Unrestricted language scripts: maximum flexibility, but unsafe and difficult to reproduce across desktop platforms.
- Native plug-ins: powerful, but introduce platform-specific binaries, ABI/version coupling, and a large trust boundary.
