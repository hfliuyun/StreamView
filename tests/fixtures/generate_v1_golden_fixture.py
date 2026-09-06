#!/usr/bin/env python3
import json
import hashlib
from pathlib import Path

def main():
    fixtures_dir = Path(__file__).parent.resolve()
    target_path = fixtures_dir / "v1_golden.svsession"

    doc = {
        "schemaVersion": 1,
        "source": {
            "fingerprint": {
                "version": 1,
                "mode": "full-content-sha256",
                "sizeBytes": "1024",
                "sha256": "4b227777d4dd1fc61c6f884f48641d02b4d121d3fd328cb08b5531fcacdabf8a"
            },
            "identity": "sample_stream.264",
            "path": "sample_stream.264"
        },
        "rule": {
            "contentSha256": "44b82ad4e0e64c1c9e47eb5bfb986ee0763ec81165bc674eb2de7387cc31f24d",
            "entryPointId": "annex-b",
            "packageId": "org.streamview.h264",
            "packageVersion": "0.1.40"
        },
        "bookmarks": [
            {
                "label": "NAL Header",
                "sourceBitOffset": "32"
            }
        ],
        "annotations": [
            {
                "bitLength": "8",
                "sourceBitOffset": "32",
                "text": "Sequence parameter set"
            }
        ],
        "expandedPaths": [
            "/0",
            "/0/0"
        ],
        "view": {
            "rawDisplayMode": "hex",
            "rawPageIndex": "0",
            "selectedAnalysisPath": "/0/0",
            "selectedSourceBitOffset": "32"
        }
    }

    content = json.dumps(doc, indent=4) + "\n"
    target_path.write_text(content, encoding="utf-8")
    print(f"Generated {target_path} successfully ({len(content)} bytes)")

if __name__ == "__main__":
    main()
