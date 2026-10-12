# P7d-3: Hosted Sanitizer Gate

The ordinary CI workflow runs the full test suite and the six deterministic
fuzz corpus replays under ASan and UBSan on Ubuntu 24.04 and macOS 15.
These jobs use the existing `sanitize` preset and Qt 6.11.1. Compiler warnings
remain errors. Sanitizer instrumentation applies to project targets through
the existing warnings helper.

Undefined behavior cannot recover: builds use `-fno-sanitize-recover=all`,
and the jobs set `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
ASan uses `halt_on_error=1`. Linux also enables LeakSanitizer; macOS disables
leak detection because the Apple platform runtime does not provide that
check. A passing macOS job proves ASan/UBSan cleanliness, not absence of leaks.
Windows MSVC AddressSanitizer remains outside this slice.

Build or test failures fail the job. Each test has a 600 second timeout and
each job has a 45 minute timeout. CTest failure details are printed into the
Actions job log, which is the retained diagnostic record. These jobs do not
upload deployment packages or test artifacts.

Local validation uses the same preset and failure options. Hosted results
must be recorded against the exact pushed commit before P7d closes.
