#!/usr/bin/env python3
"""Run independent application performance samples and preserve JSON evidence."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def create_sparse(path, prefix):
    with path.open("w+b") as source:
        if os.name == "nt":
            import msvcrt

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            control = kernel.DeviceIoControl
            control.argtypes = [
                ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                ctypes.c_void_p,
            ]
            control.restype = ctypes.c_int
            returned = ctypes.c_uint32()
            if not control(msvcrt.get_osfhandle(source.fileno()), 0x000900C4,
                           None, 0, None, 0, ctypes.byref(returned), None):
                raise ctypes.WinError(ctypes.get_last_error())
        source.write(prefix)
        source.truncate(100 * 1024**3)


def run(binary, mode, path=None):
    command = [str(binary), mode]
    if path is not None:
        command.append(str(path))
    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = "offscreen"
    completed = subprocess.run(command, env=environment, capture_output=True,
                               text=True, timeout=90, check=False)
    if completed.returncode:
        raise RuntimeError(
            f"{mode} failed ({completed.returncode}):\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    result = json.loads(completed.stdout.strip())
    print(f"{mode}: {json.dumps({k: v for k, v in result.items() if not k.endswith('samples_ms')})}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "tests" / "fixtures"
    results = []
    report = {"platform": platform.platform(), "samples": results}
    try:
        with tempfile.TemporaryDirectory(prefix="streamview-performance-") as directory:
            working = Path(directory)
            for _ in range(5):
                results.append(run(arguments.binary, "initial",
                                   fixtures / "mp4_p5j4_avc_multi_nal.mp4"))
            sparse = working / "100gib.mp4"
            prefix = (fixtures / "mp4_p5j6_100gb_sparse_prefix.bin").read_bytes()
            if len(prefix) != 677:
                raise ValueError("Unexpected sparse fixture prefix")
            create_sparse(sparse, prefix)
            for _ in range(3):
                results.append(run(arguments.binary, "sparse", sparse))
            results.append(run(arguments.binary, "save"))
            multi_batch = working / "multibatch.264"
            multi_batch.write_bytes(bytes.fromhex("0000016c") * 512)
            results.append(run(arguments.binary, "restore", multi_batch))
        report["passed"] = True
    except Exception as error:
        report["passed"] = False
        report["error"] = str(error)
        raise
    finally:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
