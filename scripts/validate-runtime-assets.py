#!/usr/bin/env python3
"""Fail the build when a runtime sound asset violates the watch decoder contract."""

from __future__ import annotations

import pathlib
import struct
import sys


ALLOWED = {
    "notification.wav",
    "call.wav",
    "calendar.wav",
    "email.wav",
    "alarm.wav",
    "timer.wav",
    "success.wav",
    "warning.wav",
}
MAX_DATA_BYTES = 5 * 16_000 * 2


def validate_wav(path: pathlib.Path) -> None:
    blob = path.read_bytes()
    if len(blob) < 44 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    if struct.unpack_from("<I", blob, 4)[0] + 8 != len(blob):
        raise ValueError("RIFF size does not match file size")

    offset = 12
    fmt = None
    data_size = None
    while offset + 8 <= len(blob):
        chunk_id = blob[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", blob, offset + 4)[0]
        payload = offset + 8
        end = payload + chunk_size
        if end > len(blob):
            raise ValueError("chunk extends beyond file")
        if chunk_id == b"fmt ":
            if fmt is not None or chunk_size != 16:
                raise ValueError("requires one 16-byte PCM fmt chunk")
            fmt = struct.unpack_from("<HHIIHH", blob, payload)
        elif chunk_id == b"data":
            if data_size is not None:
                raise ValueError("multiple data chunks are not supported")
            data_size = chunk_size
        offset = end + (chunk_size & 1)

    if offset != len(blob):
        raise ValueError("trailing or malformed chunk data")
    if fmt != (1, 1, 16_000, 32_000, 2, 16):
        raise ValueError("requires PCM16 mono at 16 kHz")
    if data_size is None or data_size == 0 or data_size > MAX_DATA_BYTES:
        raise ValueError("audio data must be between 1 sample and 5 seconds")
    if data_size % 2:
        raise ValueError("PCM16 data length must be even")


def main() -> int:
    root = pathlib.Path(sys.argv[1]).resolve()
    failures: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative.as_posix() in {"README.txt", "sounds/.gitkeep"}:
            continue
        if relative.parent.as_posix() != "sounds" or relative.name not in ALLOWED:
            failures.append(f"{relative}: asset path is not in the fixed catalog")
            continue
        try:
            validate_wav(path)
        except (OSError, ValueError, struct.error) as error:
            failures.append(f"{relative}: {error}")
    if failures:
        print("Runtime asset validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("Runtime assets validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
