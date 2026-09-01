#!/usr/bin/env python3
"""Inspect a Nightglass flash readback without changing the connected watch.

This deliberately does not invoke esptool or otatool. Entering the ROM loader
resets the watch, and a reset while an image is PENDING_VERIFY can itself cause
ESP-IDF rollback. Feed this tool an existing owner-only 32 MiB baseline image.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import pathlib
import stat
import struct


FLASH_SIZE = 32 * 1024 * 1024
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
OTADATA_OFFSET = 0xF000
OTADATA_SIZE = 0x2000
OTA_SLOT_SIZE = 0x600000
OTA_OFFSETS = (0x20000, 0x620000)
APP_DESC_OFFSET = 0x20
APP_DESC_MIN_SIZE = APP_DESC_OFFSET + 80

STATE_NAMES = {
    0: "NEW",
    1: "PENDING_VERIFY",
    2: "VALID",
    3: "INVALID",
    4: "ABORTED",
    0xFFFFFFFF: "UNDEFINED",
}


def ota_crc(sequence: int) -> int:
    return binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF


def parse_otadata(blob: bytes) -> list[dict[str, object]]:
    if len(blob) != OTADATA_SIZE:
        raise ValueError("otadata must be exactly 0x2000 bytes")
    records: list[dict[str, object]] = []
    for copy_index, offset in enumerate((0, 0x1000)):
        sequence, _label, state, crc = struct.unpack_from("<I20sII", blob, offset)
        crc_valid = sequence != 0xFFFFFFFF and crc == ota_crc(sequence)
        bootable_state = state not in (3, 4)
        records.append(
            {
                "copy": copy_index,
                "sequence": sequence,
                "state": state,
                "state_name": STATE_NAMES.get(state, f"UNKNOWN_{state:#x}"),
                "crc": crc,
                "crc_valid": crc_valid,
                "selectable": crc_valid and bootable_state,
            }
        )
    return records


def selected_boot_candidate(records: list[dict[str, object]]) -> tuple[int, int | None]:
    selectable = [record for record in records if bool(record["selectable"])]
    if not selectable:
        # Nightglass has no factory partition. ESP-IDF falls back to trying
        # ota_0 first when both otadata copies are invalid.
        return 0, None
    selected = max(selectable, key=lambda record: int(record["sequence"]))
    slot = (int(selected["sequence"]) - 1) % len(OTA_OFFSETS)
    return slot, int(selected["copy"])


def fixed_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def parse_app_description(blob: bytes) -> dict[str, object]:
    if len(blob) < APP_DESC_MIN_SIZE or blob[0] != 0xE9:
        raise ValueError("not an ESP application image")
    magic, secure_version = struct.unpack_from("<II", blob, APP_DESC_OFFSET)
    if magic != 0xABCD5432:
        raise ValueError("ESP application descriptor is missing")
    version = fixed_string(blob[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 48])
    project = fixed_string(blob[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 80])
    return {
        "project": project,
        "version": version,
        "secure_version": secure_version,
    }


def inspect_flash(
    flash: bytes, expected_partition_table: bytes, candidate: bytes | None
) -> dict[str, object]:
    if len(flash) != FLASH_SIZE:
        raise ValueError("flash readback must be exactly 32 MiB")
    live_table = flash[PARTITION_TABLE_OFFSET : PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE]
    if len(expected_partition_table) > PARTITION_TABLE_SIZE:
        raise ValueError("reviewed partition-table artifact exceeds one flash sector")
    padded_expected_table = expected_partition_table.ljust(PARTITION_TABLE_SIZE, b"\xff")
    if live_table != padded_expected_table:
        raise ValueError("device partition table differs from reviewed build artifact")

    records = parse_otadata(flash[OTADATA_OFFSET : OTADATA_OFFSET + OTADATA_SIZE])
    boot_slot, selected_copy = selected_boot_candidate(records)
    slots: list[dict[str, object]] = []
    for slot, offset in enumerate(OTA_OFFSETS):
        try:
            description = parse_app_description(flash[offset : offset + APP_DESC_MIN_SIZE])
            description.update({"slot": slot, "offset": offset})
        except (UnicodeDecodeError, ValueError) as error:
            description = {"slot": slot, "offset": offset, "error": str(error)}
        slots.append(description)

    result: dict[str, object] = {
        "flash_sha256": hashlib.sha256(flash).hexdigest(),
        "partition_table_sha256": hashlib.sha256(live_table).hexdigest(),
        "otadata": records,
        "boot_candidate_slot": boot_slot,
        "selected_otadata_copy": selected_copy,
        "inactive_candidate_slot": 1 - boot_slot,
        "slot_descriptions": slots,
        "actual_running_slot_known": False,
        "external_slot_switch_supported": False,
    }
    if candidate is not None:
        if not candidate or len(candidate) > OTA_SLOT_SIZE:
            raise ValueError("candidate is empty or exceeds a 6 MiB OTA slot")
        description = parse_app_description(candidate)
        if description["project"] != "nightglass":
            raise ValueError("candidate is not a Nightglass application")
        result["candidate"] = {
            **description,
            "size": len(candidate),
            "sha256": hashlib.sha256(candidate).hexdigest(),
        }
    return result


def print_report(report: dict[str, object]) -> None:
    print(f"flash_sha256={report['flash_sha256']}")
    print(f"partition_table_sha256={report['partition_table_sha256']}")
    for record in report["otadata"]:  # type: ignore[union-attr]
        print(
            "otadata[{copy}] seq={sequence:#010x} state={state_name} "
            "crc_valid={crc_valid} selectable={selectable}".format(**record)
        )
    selected = report["selected_otadata_copy"]
    selected_text = "fallback" if selected is None else str(selected)
    print(
        f"boot_candidate=ota_{report['boot_candidate_slot']} "
        f"selected_copy={selected_text} inactive_candidate=ota_{report['inactive_candidate_slot']}"
    )
    for slot in report["slot_descriptions"]:  # type: ignore[union-attr]
        if "error" in slot:
            print(f"ota_{slot['slot']} image={slot['error']}")
        else:
            print(
                f"ota_{slot['slot']} project={slot['project']} version={slot['version']} "
                f"secure_version={slot['secure_version']}"
            )
    candidate = report.get("candidate")
    if candidate:
        print(
            f"candidate version={candidate['version']} size={candidate['size']} "
            f"sha256={candidate['sha256']}"
        )
    print("actual_running_slot=UNKNOWN_WITH_OFFLINE_READBACK")
    print("external_slot_switch=BLOCKED_ROLLBACK_SEMANTICS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("flash_image", type=pathlib.Path)
    parser.add_argument(
        "--partition-table",
        type=pathlib.Path,
        default=pathlib.Path("build/partition_table/partition-table.bin"),
    )
    parser.add_argument("--candidate", type=pathlib.Path)
    args = parser.parse_args()
    try:
        if stat.S_IMODE(args.flash_image.stat().st_mode) & 0o077:
            raise ValueError("flash readback must be owner-only (mode 0600)")
        report = inspect_flash(
            args.flash_image.read_bytes(),
            args.partition_table.read_bytes(),
            args.candidate.read_bytes() if args.candidate else None,
        )
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise SystemExit(f"inspection refused: {error}") from None
    print_report(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
