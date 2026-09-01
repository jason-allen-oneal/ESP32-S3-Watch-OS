#!/usr/bin/env python3
"""Regression tests for authoritative Nightglass OTA HIL evidence."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest


PROJECT = pathlib.Path(__file__).resolve().parents[1]
PARSER = PROJECT / "scripts" / "ota-hil-evidence.py"


def boot(slot: str, pending: int) -> str:
    address = "0x00020000" if slot == "ota_0" else "0x00620000"
    return (f"OTA_BOOT running={slot}@{address} configured={slot}@{address} "
            f"state=2 pending={pending} rollback_possible=1\n")


class OtaHilEvidenceTest(unittest.TestCase):
    def run_parser(self, text: str, flag: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            log = pathlib.Path(directory) / "serial.log"
            log.write_text(text, encoding="utf-8")
            return subprocess.run(
                ["python3", str(PARSER), str(log), flag],
                text=True, capture_output=True, check=False)

    def test_rollback_requires_restored_prior_slot(self) -> None:
        false_positive = boot("ota_1", 1) + "requesting rollback\nrollback request failed\n"
        self.assertNotEqual(self.run_parser(false_positive, "--require-rollback").returncode, 0)
        proven = boot("ota_1", 1) + "requesting rollback\n" + boot("ota_0", 0)
        self.assertEqual(self.run_parser(proven, "--require-rollback").returncode, 0)

    def test_acceptance_requires_same_slot_stable_reboot(self) -> None:
        marker = "OTA_HEALTH_ACCEPTED state=VALID pending=0\n"
        self.assertNotEqual(self.run_parser(boot("ota_1", 1) + marker,
                                            "--require-accepted").returncode, 0)
        proven = boot("ota_1", 1) + marker + boot("ota_1", 0)
        self.assertEqual(self.run_parser(proven, "--require-accepted").returncode, 0)
        wrong_slot = boot("ota_1", 1) + marker + boot("ota_0", 0)
        self.assertNotEqual(self.run_parser(wrong_slot,
                                            "--require-accepted").returncode, 0)


if __name__ == "__main__":
    unittest.main()
