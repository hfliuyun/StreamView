# Shared Format Definition Model

The analyzer will use one loadable format-definition model for both built-in format support and user-installed analysis rules. This preserves the 010 Editor-style extensibility requested for new containers and codecs while keeping field locations, decoded values, and explanations consistent across the UI; the execution model must be constrained so rules cannot freely access the host system or network.

**Considered Options**

- Built-in parsers only: simpler to ship, but every new format requires an application release.
- Arbitrary native plug-ins: flexible, but difficult to keep portable, safe, and reproducible across desktop systems.
