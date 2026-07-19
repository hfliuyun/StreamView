# Tiered Desktop Platform Support

StreamView will target desktop platforms through a tiered support promise. Windows 11 x64, macOS 14+ on Apple Silicon, and Ubuntu 24.04 LTS x64 are the primary validated tier; Windows 10, Intel Macs, and other modern x64 Linux distributions are the secondary compatibility tier. This keeps the cross-platform core intact while making testing and release guarantees finite and explicit.

**Considered Options**

- Promise support for every desktop OS and distribution: not objectively testable or maintainable.
- Support one operating system first: simpler validation, but conflicts with the product's cross-platform requirement and makes later portability more expensive.
