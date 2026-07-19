# Separate Persistent Analysis Sessions

Bookmarks, annotations, selected rule versions, and navigation state will be stored in a separate saved session while media sources remain strictly read-only. A source fingerprint will prevent saved locations from being silently applied to changed media, and rebuildable large indexes will be managed outside the compact session record. This preserves user work without turning the analyzer into a media editor or duplicating large source files.
