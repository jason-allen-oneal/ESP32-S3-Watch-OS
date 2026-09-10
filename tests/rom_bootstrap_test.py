#!/usr/bin/env python3
"""Exercise the one-time upgrade transaction without connecting to hardware."""

from __future__ import annotations

from dataclasses import replace
import hashlib
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import nightglass_rom_bootstrap as bootstrap


def image(version: str, secure_version: int) -> bytes:
    data = bytearray(512)
    data[0] = bootstrap.ESP_IMAGE_MAGIC
    struct.pack_into("<II", data, 0x20, bootstrap.APP_DESCRIPTOR_MAGIC, secure_version)
    data[0x30:0x30 + len(version)] = version.encode()
    data[0x50:0x5A] = b"nightglass"
    return bytes(data)


def ota_record(sequence: int, state: int) -> bytes:
    return struct.pack("<I20sII", sequence, b"\xff" * 20, state,
                       bootstrap.ota_crc(sequence)).ljust(4096, b"\xff")


class MemoryFlash:
    def __init__(self, *, active_copy: int = 1) -> None:
        self.flash = bytearray(b"\xff" * (bootstrap.OTA_1_OFFSET + 512))
        self.writes: list[tuple[int, int]] = []
        self.erases: list[tuple[int, int]] = []
        self.corrupt_image = False
        self.corrupt_metadata = False
        self.table = b"reviewed-partition-table"
        self.put(bootstrap.PARTITION_TABLE_OFFSET, self.table)
        self.put(bootstrap.NVS_OFFSET, b"preserved-pairing-settings" * 100)
        records = [ota_record(1, bootstrap.ESP_OTA_IMG_VALID),
                   ota_record(2, bootstrap.ESP_OTA_IMG_VALID)]
        if active_copy == 0:
            records.reverse()
        self.put(bootstrap.OTADATA_OFFSET, b"".join(records))
        self.put(bootstrap.OTA_1_OFFSET, image("0.2.2", 2))

    def put(self, address: int, data: bytes) -> None:
        self.flash[address:address + len(data)] = data

    def read(self, address: int, size: int) -> bytes:
        return bytes(self.flash[address:address + size])

    def write(self, address: int, data: bytes, name: str) -> None:
        self.writes.append((address, len(data)))
        size = bootstrap.align_up(len(data), bootstrap.FLASH_SECTOR_SIZE)
        self.put(address, b"\xff" * size)
        self.put(address, data)
        if address == bootstrap.OTA_0_OFFSET and self.corrupt_image:
            self.flash[address + 100] ^= 1
        if address < bootstrap.OTA_0_OFFSET and self.corrupt_metadata:
            self.flash[address + 28] ^= 1

    def erase(self, address: int, size: int) -> None:
        self.erases.append((address, size))
        self.put(address, b"\xff" * size)


class BootstrapTransactionTest(unittest.TestCase):
    def setUp(self) -> None:
        firmware = image("0.2.4", 4)
        self.package = bootstrap.BootstrapPackage(
            "0.2.4", 4, len(firmware), hashlib.sha256(firmware).digest(), firmware)
        self.evidence = bootstrap.RunningEvidence(1, bootstrap.OTA_1_OFFSET,
                                                   1, bootstrap.OTA_1_OFFSET,
                                                   bootstrap.ESP_OTA_IMG_VALID,
                                                   False, True)

    def test_commit_preserves_all_bytes_outside_inactive_targets(self) -> None:
        for active_copy in (0, 1):
            with self.subTest(active_copy=active_copy):
                flash = MemoryFlash(active_copy=active_copy)
                before = bytes(flash.flash)
                plan = bootstrap.perform_bootstrap(flash, self.evidence,
                                                   self.package, flash.table)
                self.assertEqual(flash.writes, [(bootstrap.OTA_0_OFFSET, 512),
                                               (plan.target_metadata_offset, 32)])
                expected = bytearray(before)
                expected[bootstrap.OTA_0_OFFSET:bootstrap.OTA_0_OFFSET + 512] = self.package.firmware
                start = plan.target_metadata_offset
                expected[start:start + 4096] = plan.metadata_record.ljust(4096, b"\xff")
                self.assertEqual(flash.flash, expected)
                records = bootstrap.parse_otadata(flash.read(bootstrap.OTADATA_OFFSET, 8192))
                selected = bootstrap.select_active_record(records)
                self.assertEqual((selected.slot, selected.state), (0, bootstrap.ESP_OTA_IMG_NEW))
                self.assertEqual(records[active_copy].state, bootstrap.ESP_OTA_IMG_VALID)

    def test_failed_image_readback_never_changes_boot_selection(self) -> None:
        flash = MemoryFlash()
        flash.corrupt_image = True
        before = flash.read(bootstrap.OTADATA_OFFSET, 8192)
        with self.assertRaisesRegex(bootstrap.PreservationError, "readback"):
            bootstrap.perform_bootstrap(flash, self.evidence, self.package, flash.table)
        self.assertEqual(flash.read(bootstrap.OTADATA_OFFSET, 8192), before)
        self.assertEqual(len(flash.writes), 1)

    def test_retry_after_rollback_preserves_the_remaining_valid_image(self) -> None:
        flash = MemoryFlash()
        flash.put(bootstrap.OTADATA_OFFSET, ota_record(3, bootstrap.ESP_OTA_IMG_ABORTED))
        old = flash.read(bootstrap.OTADATA_OFFSET + 4096, 4096)
        bootstrap.perform_bootstrap(flash, replace(self.evidence, rollback_possible=False),
                                    self.package, flash.table)
        self.assertEqual(flash.read(bootstrap.OTADATA_OFFSET + 4096, 4096), old)

    def test_failed_metadata_commit_restores_old_boot_selection(self) -> None:
        flash = MemoryFlash()
        flash.corrupt_metadata = True
        with self.assertRaises(bootstrap.PreservationError):
            bootstrap.perform_bootstrap(flash, self.evidence, self.package, flash.table)
        selected = bootstrap.select_active_record(bootstrap.parse_otadata(
            flash.read(bootstrap.OTADATA_OFFSET, 8192)))
        self.assertEqual((selected.slot, selected.state), (1, bootstrap.ESP_OTA_IMG_VALID))
        self.assertEqual(flash.erases, [(bootstrap.OTADATA_OFFSET, 4096)])

    def test_pending_or_wrong_source_is_rejected_before_any_write(self) -> None:
        for bad in (replace(self.evidence, pending=True),
                    replace(self.evidence, running_slot=0)):
            flash = MemoryFlash()
            with self.assertRaises(bootstrap.EvidenceError):
                bootstrap.perform_bootstrap(flash, bad, self.package, flash.table)
            self.assertEqual(flash.writes, [])
        flash = MemoryFlash()
        flash.put(bootstrap.OTA_1_OFFSET, image("0.2.3", 3))
        with self.assertRaises(bootstrap.OtaStateError):
            bootstrap.perform_bootstrap(flash, self.evidence, self.package, flash.table)
        self.assertEqual(flash.writes, [])

    def test_partition_mismatch_is_rejected_before_any_write(self) -> None:
        flash = MemoryFlash()
        with self.assertRaises(bootstrap.PreservationError):
            bootstrap.perform_bootstrap(flash, self.evidence, self.package, b"other-layout")
        self.assertEqual(flash.writes, [])

    def test_health_acceptance_requires_usb_and_pending_boot_evidence(self) -> None:
        tracker = bootstrap.HealthLogTracker()
        tracker.feed(b"OTA_HEALTH_ACCEPTED state=VALID pending=0\n")
        self.assertFalse(tracker.accepted)
        tracker.feed(b"OTA_BOOT running=ota_0@0x00020000 configured=ota_0@0x00020000 "
                     b"state=1 pending=1 rollback_possible=1\n")
        tracker.feed(b"USB_UPDATE_BOOT version=0.2.4 secure=4 pending=1\n")
        tracker.feed(bootstrap._EXPECTED_USB_READY + b"\n")
        self.assertFalse(tracker.accepted, "old acceptance cannot validate a later boot")
        tracker.feed(b"OTA_HEALTH_ACCEPTED state=VALID pending=0\n")
        self.assertTrue(tracker.accepted)
        with self.assertRaisesRegex(bootstrap.BootstrapError, "rolled back"):
            tracker.feed(b"OTA_BOOT running=ota_1@0x00620000 configured=ota_1@0x00620000 "
                         b"state=2 pending=0 rollback_possible=1\n")

    def test_health_wait_probes_the_new_usb_receiver_without_reset(self) -> None:
        port = mock.create_autospec(bootstrap.PosixSerialPort, instance=True)
        first = True

        def read(timeout: float) -> bytes:
            nonlocal first
            if first:
                first = False
                return (b"OTA_BOOT running=ota_0@0x00020000 configured=ota_0@0x00020000 "
                        b"state=1 pending=1 rollback_possible=1\n" +
                        bootstrap._EXPECTED_USB_READY + b"\n")
            port.write_all.assert_called_once()
            return (b"USB_UPDATE_BOOT version=0.2.4 secure=4 pending=1\n"
                    b"OTA_HEALTH_ACCEPTED state=VALID pending=0\n")

        port.read.side_effect = read
        with mock.patch.object(bootstrap, "PosixSerialPort", return_value=port):
            tracker = bootstrap.wait_for_health(Path("/test-watch"), 1.0, mirror_serial=False)
        self.assertTrue(tracker.accepted)
        port.close.assert_called_once()

    def test_rom_preflight_attaches_flash_and_resets_on_failure(self) -> None:
        esp = mock.Mock(CHIP_NAME="ESP32-S3", IMAGE_CHIP_ID=9)
        esp.get_security_info.return_value = {"parsed_flags": {}, "flash_crypt_cnt": 0}
        esp.run_stub.return_value = esp

        def flash_id() -> int:
            esp.flash_spi_attach.assert_called_once_with(0)
            return 0x2040EF

        esp.flash_id.side_effect = flash_id
        commands = SimpleNamespace(DETECTED_FLASH_SIZES={0x20: "32MB"},
                                   detect_chip=mock.Mock(return_value=esp))
        with mock.patch.dict(sys.modules, {"esptool.cmds": commands}):
            session = bootstrap.EsptoolRomSession(Path("/test-watch"), 115200)
            esp.flash_set_parameters.assert_called_once_with(bootstrap.FLASH_SIZE)
            esp.hard_reset.assert_not_called()
            session.close()

            esp.reset_mock()
            esp.flash_id.side_effect = RuntimeError("probe failed")
            with self.assertRaisesRegex(RuntimeError, "probe failed"):
                bootstrap.EsptoolRomSession(Path("/test-watch"), 115200)
            esp.hard_reset.assert_called_once()
            esp._port.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
