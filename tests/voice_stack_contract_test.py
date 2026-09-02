#!/usr/bin/env python3
"""Fail closed when the proven voice transport stack contract regresses."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components" / "nightglass_services" / "src" / "voice.cpp"


def constant(source: str, name: str) -> int:
    match = re.search(
        rf"constexpr\s+(?:std::uint32_t|UBaseType_t)\s+{name}\s*=\s*([0-9']+)\s*;",
        source,
    )
    assert match is not None, f"missing {name}"
    return int(match.group(1).replace("'", ""))


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    assert constant(source, "kWorkerStackBytes") >= 8192
    assert constant(source, "kWorkerMinimumStackReserveBytes") >= 2048
    creation = re.search(
        r'xTaskCreateWithCaps\(voice_worker_task,\s*"ng_voice",'
        r'.*?kWorkerStackBytes.*?MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\)',
        source,
        re.DOTALL,
    )
    assert creation is not None, "ng_voice stack must remain PSRAM-backed"
    assert "uxTaskGetStackHighWaterMark(nullptr)" in source
    print("Nightglass voice stack contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
