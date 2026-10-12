# P7e: Performance Measurement Contract

The opt-in `performance` preset builds a dedicated application benchmark.
Measurements run in fresh processes using the actual `MainWindow`,
`FileSource` and `SourcePager` paths, with offscreen Qt rendering for
repeatability. Offscreen painting is a rendering regression check, not a
human assessment of the desktop display.

The benchmark records JSON and exits nonzero on a failed invariant or budget:

- Initial view: five fresh processes opening
  `mp4_p5j4_avc_multi_nal.mp4`, from before `QApplication` construction until
  the root model is populated and the visible window is painted. Each must
  finish in at most 2 seconds.
- Large source: three fresh processes opening a 100 GiB disk sparse MP4 made
  from the existing 677-byte fixture prefix. Opening includes the real
  sampled fingerprint, scanning, root model and painting. Peak resident
  memory must be at most 512 MiB. Windows marks the file sparse before
  extending it.
- Page reads: 1,000 deterministic, dispersed 64 KiB reads in the large file.
  All reads must succeed with the requested offset and size. Nearest-rank p95
  must be strictly below 100 ms. No warm-up samples are discarded.
- Save traversal: a synthetic materialized tree of at least 10,000 nodes
  attached to the actual window model, with repeated
  `MainWindow::currentUserState()` calls. Maximum duration must be at most
  50 ms. Node count and expanded state must be reported.
- Restoration: a generated Annex B file requiring multiple analysis batches.
  Save a late tree selection and expansion, restore in a fresh window, and
  verify the pending paths drain to that exact node. Report elapsed time and
  maximum event-loop heartbeat gap, with a finite timeout. This is an
  observation metric; it does not invent a new numerical budget.

Ordinary correctness CTest remains independent of this opt-in timing gate.
Run on an otherwise idle machine and record platform, Qt version, build
configuration and each sample. ASan/UBSan timings must be labelled separately
from release measurements.
