#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import pathlib
import struct
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "ota_state_inspector", ROOT / "scripts" / "inspect-ota-state.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def entry(sequence: int, state: int, valid_crc: bool = True) -> bytes:
    crc = MODULE.ota_crc(sequence)
    if not valid_crc:
        crc ^= 1
    return struct.pack("<I20sII", sequence, bytes(20), state, crc) + bytes(0x1000 - 32)


class OtaStateInspectorTest(unittest.TestCase):
    def test_valid_slot_one_is_selected_and_slot_zero_is_inactive(self) -> None:
        records = MODULE.parse_otadata(entry(1, 2) + entry(2, 2))
        self.assertEqual(MODULE.selected_boot_candidate(records), (1, 1))

    def test_pending_verify_is_reported_and_remains_selected(self) -> None:
        records = MODULE.parse_otadata(entry(3, 1) + entry(2, 2))
        self.assertEqual(records[0]["state_name"], "PENDING_VERIFY")
        self.assertEqual(MODULE.selected_boot_candidate(records), (0, 0))

    def test_aborted_or_bad_crc_records_are_not_selected(self) -> None:
        records = MODULE.parse_otadata(entry(3, 4) + entry(2, 2, valid_crc=False))
        self.assertEqual(MODULE.selected_boot_candidate(records), (0, None))

    def test_tie_matches_esp_idf_first_copy_selection(self) -> None:
        records = MODULE.parse_otadata(entry(5, 2) + entry(5, 2))
        self.assertEqual(MODULE.selected_boot_candidate(records), (0, 0))

    def test_rejects_wrong_otadata_size(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly 0x2000"):
            MODULE.parse_otadata(bytes(32))

    def test_partition_table_artifact_is_ff_padded_to_its_flash_sector(self) -> None:
        table_artifact = b"reviewed-table"
        flash = bytearray(b"\xff" * MODULE.FLASH_SIZE)
        start = MODULE.PARTITION_TABLE_OFFSET
        flash[start : start + len(table_artifact)] = table_artifact
        ota = entry(1, 2) + entry(2, 2)
        flash[MODULE.OTADATA_OFFSET : MODULE.OTADATA_OFFSET + len(ota)] = ota
        for offset in MODULE.OTA_OFFSETS:
            flash[offset] = 0
        report = MODULE.inspect_flash(bytes(flash), table_artifact, None)
        self.assertEqual(report["boot_candidate_slot"], 1)


if __name__ == "__main__":
    unittest.main()
