# Local Files Before Live Sources

The first release will analyze only complete, randomly accessible local files, including containers and standalone elementary streams. Network URLs, capture devices, pipes, and other progressively arriving sources are deferred until the file-analysis model is stable because they require separate buffering, loss/reordering, incomplete-input, and position-semantics decisions.
