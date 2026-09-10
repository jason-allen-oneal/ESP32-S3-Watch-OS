#!/usr/bin/env python3
"""One-time, rollback-preserving ROM bootstrap for Nightglass 0.2.2.

This is deliberately narrower than a generic ESP flasher.  It accepts only a
signed Nightglass 0.2.4 package, only a watch proven to be running the valid
0.2.2 image in ``ota_1``, and only writes the inactive ``ota_0`` application
slot plus one inactive OTA metadata sector.  NVS, the running application,
bootloader, partition table, PHY data, coredump, and assets are never write
targets.

The application is written and read back before OTA metadata is committed.
The old valid metadata record remains intact, so loss of power before or during
the final metadata write still leaves a bootable 0.2.2 rollback image.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import io
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Protocol

from nightglass_usb_update import (
    AckTimeout,
    DevicePathError,
    LinkDisconnected,
    PackageError,
    PosixSerialPort,
    SignedPackage,
    UPDATE_STATUS_QUERY,
    build_control_payload,
    competing_device_holders,
    encode_envelope,
    load_signed_package,
    validate_stable_serial_path,
)


FLASH_SIZE = 32 * 1024 * 1024
FLASH_SECTOR_SIZE = 0x1000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0x1000
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
OTADATA_OFFSET = 0xF000
OTADATA_SIZE = 0x2000
OTA_0_OFFSET = 0x20000
OTA_1_OFFSET = 0x620000
OTA_SLOT_SIZE = 0x600000
OTA_0_END = OTA_0_OFFSET + OTA_SLOT_SIZE
APP_DESCRIPTOR_OFFSET = 0x20
APP_DESCRIPTOR_READ_SIZE = 0x120
APP_DESCRIPTOR_MAGIC = 0xABCD5432
ESP_IMAGE_MAGIC = 0xE9
ESP32S3_IMAGE_CHIP_ID = 9

EXPECTED_SOURCE_VERSION = "0.2.2"
EXPECTED_SOURCE_SECURE_VERSION = 2
EXPECTED_TARGET_VERSION = "0.2.4"
EXPECTED_TARGET_SECURE_VERSION = 4
EXPECTED_USB_NAME_PREFIX = "usb-Espressif_USB_JTAG_serial_debug_unit_"
EXPECTED_USB_NAME_SUFFIX = "-if00"

ESP_OTA_IMG_NEW = 0
ESP_OTA_IMG_PENDING_VERIFY = 1
ESP_OTA_IMG_VALID = 2
ESP_OTA_IMG_INVALID = 3
ESP_OTA_IMG_ABORTED = 4
ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF

_OTA_RECORD = struct.Struct("<I20sII")
_OTA_BOOT = re.compile(
    r"OTA_BOOT\s+running=(ota_[01])@(0x[0-9a-fA-F]+)\s+"
    r"configured=(ota_[01])@(0x[0-9a-fA-F]+)\s+state=(\d+)\s+"
    r"pending=([01])\s+rollback_possible=([01])"
)
_EXPECTED_PENDING_BOOT = re.compile(
    rb"OTA_BOOT\s+running=ota_0@0x0*20000\s+"
    rb"configured=ota_0@0x0*20000\s+state=1\s+pending=1\s+"
    rb"rollback_possible=1"
)
_EXPECTED_USB_BOOT = re.compile(
    rb"USB_UPDATE_BOOT[^\r\n]*\bversion=0\.2\.4\b"
    rb"[^\r\n]*\bsecure=4\b[^\r\n]*\bpending=1\b"
)
_EXPECTED_USB_READY = (
    b"USB_UPDATE_READY protocol=1 signed_only=1 max_chunk=230 raw_flash=0"
)
_HEALTH_ACCEPTED = b"OTA_HEALTH_ACCEPTED state=VALID pending=0"
_ROLLBACK_MARKER = re.compile(
    rb"pending image failed health gate|requesting rollback", re.IGNORECASE
)
_OLD_SLOT_BOOT = re.compile(
    rb"OTA_BOOT\s+running=ota_1@0x0*620000\s+"
    rb"configured=ota_1@0x0*620000\s+state=2\s+pending=0"
)


class BootstrapError(RuntimeError):
    """The one-time bootstrap refused or failed safely."""


class EvidenceError(BootstrapError):
    """The supplied live running-slot evidence is not safe for this bootstrap."""


class OtaStateError(BootstrapError):
    """The device OTA metadata does not match the live running-slot evidence."""


class PreservationError(BootstrapError):
    """A protected flash region changed during the ROM transaction."""


@dataclass(frozen=True)
class RunningEvidence:
    running_slot: int
    running_offset: int
    configured_slot: int
    configured_offset: int
    state: int
    pending: bool
    rollback_possible: bool


@dataclass(frozen=True)
class AppDescriptor:
    version: str
    project_name: str
    secure_version: int


@dataclass(frozen=True)
class OtaRecord:
    copy: int
    sequence: int
    label: bytes
    state: int
    crc: int
    crc_valid: bool
    selectable: bool
    raw_sector: bytes

    @property
    def slot(self) -> int | None:
        if self.sequence in (0, 0xFFFFFFFF):
            return None
        return (self.sequence - 1) % 2


@dataclass(frozen=True)
class BootstrapPackage:
    app_version: str
    secure_version: int
    image_size: int
    image_sha256: bytes
    firmware: bytes


@dataclass(frozen=True)
class BootstrapPlan:
    active_copy: int
    active_sequence: int
    target_copy: int
    target_metadata_offset: int
    target_app_offset: int
    new_sequence: int
    new_crc: int
    metadata_record: bytes


class RomSession(Protocol):
    def read(self, address: int, size: int) -> bytes: ...

    def write(self, address: int, data: bytes, name: str) -> None: ...

    def erase(self, address: int, size: int) -> None: ...

    def hard_reset(self) -> None: ...

    def close(self) -> None: ...


def ota_crc(sequence: int) -> int:
    """Match ``esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4)`` exactly."""

    if not 0 <= sequence <= 0xFFFFFFFF:
        raise ValueError("OTA sequence is outside uint32")
    return binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF


def parse_running_evidence(text: str) -> RunningEvidence:
    matches = list(_OTA_BOOT.finditer(text))
    if len(matches) != 1:
        raise EvidenceError("running evidence must contain exactly one OTA_BOOT line")
    match = matches[0]
    running_label, running_address, configured_label, configured_address = match.group(1, 2, 3, 4)
    evidence = RunningEvidence(
        running_slot=int(running_label[-1]),
        running_offset=int(running_address, 16),
        configured_slot=int(configured_label[-1]),
        configured_offset=int(configured_address, 16),
        state=int(match.group(5)),
        pending=match.group(6) == "1",
        rollback_possible=match.group(7) == "1",
    )
    validate_running_evidence(evidence)
    return evidence


def validate_running_evidence(evidence: RunningEvidence) -> None:
    if (
        evidence.running_slot != 1
        or evidence.running_offset != OTA_1_OFFSET
        or evidence.configured_slot != 1
        or evidence.configured_offset != OTA_1_OFFSET
    ):
        raise EvidenceError("bootstrap requires running and configured ota_1 at 0x620000")
    if evidence.state != ESP_OTA_IMG_VALID or evidence.pending:
        raise EvidenceError("running ota_1 must be VALID and not pending verification")
    # The other slot may be aborted after a failed upgrade. The valid running
    # ota_1 image becomes the new image's rollback target; it does not need its
    # own older rollback target. make_bootstrap_plan checks its actual record.


def _fixed_string(field: bytes, name: str) -> str:
    terminator = field.find(b"\0")
    if terminator <= 0 or any(field[terminator + 1 :]):
        raise BootstrapError(f"application {name} is not uniquely NUL padded")
    try:
        value = field[:terminator].decode("ascii")
    except UnicodeDecodeError as error:
        raise BootstrapError(f"application {name} is not ASCII") from error
    if any(ord(character) < 0x21 or ord(character) > 0x7E for character in value):
        raise BootstrapError(f"application {name} contains non-canonical text")
    return value


def parse_app_descriptor(image_prefix: bytes) -> AppDescriptor:
    if len(image_prefix) < APP_DESCRIPTOR_OFFSET + 80 or image_prefix[0] != ESP_IMAGE_MAGIC:
        raise BootstrapError("not an ESP application image")
    magic, secure_version = struct.unpack_from("<II", image_prefix, APP_DESCRIPTOR_OFFSET)
    if magic != APP_DESCRIPTOR_MAGIC:
        raise BootstrapError("ESP application descriptor is missing")
    version = _fixed_string(
        image_prefix[APP_DESCRIPTOR_OFFSET + 16 : APP_DESCRIPTOR_OFFSET + 48],
        "version",
    )
    project_name = _fixed_string(
        image_prefix[APP_DESCRIPTOR_OFFSET + 48 : APP_DESCRIPTOR_OFFSET + 80],
        "project name",
    )
    return AppDescriptor(version, project_name, secure_version)


def freeze_signed_package(package: SignedPackage) -> BootstrapPackage:
    firmware = package.firmware_path.read_bytes()
    digest = hashlib.sha256(firmware).digest()
    if len(firmware) != package.image_size or digest != package.image_sha256:
        raise PackageError("firmware changed after signed-package verification")
    frozen = BootstrapPackage(
        app_version=package.app_version,
        secure_version=package.secure_version,
        image_size=package.image_size,
        image_sha256=package.image_sha256,
        firmware=firmware,
    )
    validate_target_package(frozen)
    return frozen


def validate_target_package(package: BootstrapPackage) -> AppDescriptor:
    if package.app_version != EXPECTED_TARGET_VERSION:
        raise PackageError("ROM bootstrap accepts only Nightglass 0.2.4")
    if package.secure_version != EXPECTED_TARGET_SECURE_VERSION:
        raise PackageError("ROM bootstrap requires secure version 4")
    if package.image_size != len(package.firmware) or not 0 < package.image_size <= OTA_SLOT_SIZE:
        raise PackageError("signed firmware size is outside ota_0")
    if hashlib.sha256(package.firmware).digest() != package.image_sha256:
        raise PackageError("frozen firmware SHA-256 differs from signed manifest")
    descriptor = parse_app_descriptor(package.firmware[:APP_DESCRIPTOR_READ_SIZE])
    if descriptor.project_name != "nightglass":
        raise PackageError("signed firmware is not a Nightglass application")
    if descriptor.version != package.app_version:
        raise PackageError("embedded version differs from signed manifest")
    if descriptor.secure_version != package.secure_version:
        raise PackageError("embedded secure version differs from signed manifest")
    erase_end = OTA_0_OFFSET + align_up(package.image_size, FLASH_SECTOR_SIZE)
    if erase_end > OTA_0_END:
        raise PackageError("firmware erase footprint exceeds ota_0")
    return descriptor


def align_up(value: int, alignment: int) -> int:
    if value < 0 or alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")
    return (value + alignment - 1) & ~(alignment - 1)


def parse_otadata(blob: bytes) -> tuple[OtaRecord, OtaRecord]:
    if len(blob) != OTADATA_SIZE:
        raise OtaStateError("otadata must be exactly 0x2000 bytes")
    records: list[OtaRecord] = []
    for copy in range(2):
        sector = blob[copy * FLASH_SECTOR_SIZE : (copy + 1) * FLASH_SECTOR_SIZE]
        sequence, label, state, crc = _OTA_RECORD.unpack_from(sector)
        crc_valid = sequence != 0xFFFFFFFF and crc == ota_crc(sequence)
        selectable = crc_valid and state not in (ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED)
        records.append(
            OtaRecord(copy, sequence, label, state, crc, crc_valid, selectable, sector)
        )
    return records[0], records[1]


def select_active_record(records: tuple[OtaRecord, OtaRecord]) -> OtaRecord:
    selectable = [record for record in records if record.selectable]
    if not selectable:
        raise OtaStateError("no selectable OTA metadata record")
    if len(selectable) == 2 and selectable[0].sequence == selectable[1].sequence:
        raise OtaStateError("equal OTA sequences are ambiguous and refused")
    return max(selectable, key=lambda record: record.sequence)


def make_bootstrap_plan(
    evidence: RunningEvidence,
    otadata: bytes,
    running_descriptor: AppDescriptor,
    package: BootstrapPackage,
) -> BootstrapPlan:
    validate_running_evidence(evidence)
    validate_target_package(package)
    if (
        running_descriptor.project_name != "nightglass"
        or running_descriptor.version != EXPECTED_SOURCE_VERSION
        or running_descriptor.secure_version != EXPECTED_SOURCE_SECURE_VERSION
    ):
        raise OtaStateError("ota_1 is not the expected Nightglass 0.2.2 secure-version 2 image")
    if package.secure_version <= running_descriptor.secure_version:
        raise OtaStateError("target secure version is not greater than the running image")

    records = parse_otadata(otadata)
    if any(record.state == ESP_OTA_IMG_PENDING_VERIFY for record in records):
        raise OtaStateError("otadata contains a pending-verification record")
    if any(
        record.state not in (
            ESP_OTA_IMG_NEW,
            ESP_OTA_IMG_VALID,
            ESP_OTA_IMG_INVALID,
            ESP_OTA_IMG_ABORTED,
            ESP_OTA_IMG_UNDEFINED,
        )
        for record in records
    ):
        raise OtaStateError("otadata contains an unknown state")
    active = select_active_record(records)
    if active.state != ESP_OTA_IMG_VALID or active.slot != 1:
        raise OtaStateError("selected OTA record is not valid ota_1")
    if active.sequence == 0 or active.sequence >= 0xFFFFFFFE:
        raise OtaStateError("active OTA sequence cannot be advanced safely")

    new_sequence = active.sequence + 1
    if new_sequence == 0xFFFFFFFF or (new_sequence - 1) % 2 != 0:
        raise OtaStateError("next OTA sequence does not select ota_0")
    target_copy = active.copy ^ 1
    crc = ota_crc(new_sequence)
    metadata_record = _OTA_RECORD.pack(
        new_sequence, b"\xff" * 20, ESP_OTA_IMG_NEW, crc
    )
    return BootstrapPlan(
        active_copy=active.copy,
        active_sequence=active.sequence,
        target_copy=target_copy,
        target_metadata_offset=OTADATA_OFFSET + target_copy * FLASH_SECTOR_SIZE,
        target_app_offset=OTA_0_OFFSET,
        new_sequence=new_sequence,
        new_crc=crc,
        metadata_record=metadata_record,
    )


def verify_partition_table(live_sector: bytes, reviewed_artifact: bytes) -> None:
    if len(live_sector) != PARTITION_TABLE_SIZE:
        raise PreservationError("live partition-table sector has the wrong size")
    if not reviewed_artifact or len(reviewed_artifact) > PARTITION_TABLE_SIZE:
        raise PreservationError("reviewed partition-table artifact has the wrong size")
    expected = reviewed_artifact.ljust(PARTITION_TABLE_SIZE, b"\xff")
    if live_sector != expected:
        raise PreservationError("device partition table differs from reviewed revision 1")


def verify_unchanged(before: bytes, after: bytes, name: str) -> None:
    if len(before) != len(after) or not hashlib.sha256(before).digest() == hashlib.sha256(after).digest():
        raise PreservationError(f"{name} changed during the ROM transaction")


def expected_committed_otadata(before: bytes, plan: BootstrapPlan) -> bytes:
    expected = bytearray(before)
    sector_start = plan.target_copy * FLASH_SECTOR_SIZE
    expected[sector_start : sector_start + FLASH_SECTOR_SIZE] = b"\xff" * FLASH_SECTOR_SIZE
    expected[sector_start : sector_start + len(plan.metadata_record)] = plan.metadata_record
    return bytes(expected)


def verify_metadata_commit(before: bytes, after: bytes, plan: BootstrapPlan) -> None:
    if after != expected_committed_otadata(before, plan):
        raise PreservationError("OTA metadata commit did not match the exact NEW record")
    before_records = parse_otadata(before)
    after_records = parse_otadata(after)
    if after_records[plan.active_copy].raw_sector != before_records[plan.active_copy].raw_sector:
        raise PreservationError("active rollback metadata sector changed")
    committed = after_records[plan.target_copy]
    if (
        committed.sequence != plan.new_sequence
        or committed.state != ESP_OTA_IMG_NEW
        or committed.crc != plan.new_crc
        or committed.slot != 0
    ):
        raise PreservationError("committed metadata does not select ota_0 as NEW")


def validate_security_info(chip_name: str, flash_size: str | None, info: dict[str, object]) -> None:
    if chip_name != "ESP32-S3":
        raise BootstrapError(f"connected chip is {chip_name}, not ESP32-S3")
    if flash_size != "32MB":
        raise BootstrapError(f"connected flash is {flash_size or 'unknown'}, not 32MB")
    parsed = info.get("parsed_flags")
    if not isinstance(parsed, dict):
        raise BootstrapError("esptool did not return parsed security flags")
    if parsed.get("SECURE_BOOT_EN"):
        raise BootstrapError("Secure Boot is enabled; plaintext ROM bootstrap refused")
    crypt_count = info.get("flash_crypt_cnt")
    if type(crypt_count) is not int:
        raise BootstrapError("esptool did not return SPI_BOOT_CRYPT_CNT")
    if bin(crypt_count).count("1") % 2:
        raise BootstrapError("flash encryption is enabled; plaintext ROM bootstrap refused")
    if parsed.get("SECURE_DOWNLOAD_ENABLE"):
        raise BootstrapError("secure download mode prevents preservation readback")
    if parsed.get("DIS_USB"):
        raise BootstrapError("USB access is disabled")


def validate_bootstrap_device_path(path: Path) -> Path:
    candidate = validate_stable_serial_path(path)
    if not (
        candidate.name.startswith(EXPECTED_USB_NAME_PREFIX)
        and candidate.name.endswith(EXPECTED_USB_NAME_SUFFIX)
    ):
        raise DevicePathError("bootstrap requires the watch's Espressif USB-JTAG by-id path")
    holders = competing_device_holders(candidate)
    if holders:
        raise DevicePathError(
            "serial device is already open by process id(s) "
            + ", ".join(str(pid) for pid in holders)
        )
    return candidate


class _NamedBytesIO(io.BytesIO):
    def __init__(self, data: bytes, name: str) -> None:
        super().__init__(data)
        self.name = name


class EsptoolRomSession:
    """A single esptool connection kept alive for the complete transaction."""

    def __init__(self, path: Path, baud: int) -> None:
        try:
            from esptool.cmds import DETECTED_FLASH_SIZES, detect_chip
        except ImportError as error:
            raise BootstrapError(
                "esptool is unavailable; source ESP-IDF 5.5.5 export.sh first"
            ) from error

        self._esp = detect_chip(
            port=os.fspath(path),
            baud=115200,
            connect_mode="default_reset",
            connect_attempts=7,
        )
        try:
            security = self._esp.get_security_info(cache=False)
            # The CLI normally attaches the board's SPI flash before probing
            # its JEDEC ID. detect_chip() alone only connects to the ROM.
            self._esp.flash_spi_attach(0)
            flash_id = self._esp.flash_id()
            flash_size = DETECTED_FLASH_SIZES.get((flash_id >> 16) & 0xFF)
            validate_security_info(self._esp.CHIP_NAME, flash_size, security)
            if self._esp.IMAGE_CHIP_ID != ESP32S3_IMAGE_CHIP_ID:
                raise BootstrapError("esptool ESP32-S3 chip identifier is unexpected")
            self._esp = self._esp.run_stub()
            self._esp.flash_set_parameters(FLASH_SIZE)
            if baud != 115200:
                self._esp.change_baud(baud)
        except Exception:
            # Construction failures otherwise leave no session for main()'s
            # cleanup, stranding the watch in the ROM download screen.
            try:
                self.hard_reset()
            except Exception:
                pass
            self.close()
            raise

    def read(self, address: int, size: int) -> bytes:
        if address < 0 or size <= 0 or address + size > FLASH_SIZE:
            raise BootstrapError("flash read is outside the 32MB device")
        last_percent = -20

        def progress(received: int, total: int) -> None:
            nonlocal last_percent
            percent = min(100, received * 100 // total)
            if percent == last_percent or (percent < 100 and percent - last_percent < 20):
                return
            last_percent = percent
            print(f"Application readback: {percent}%", file=sys.stderr)

        data = self._esp.read_flash(address, size, progress if size >= 512 * 1024 else None)
        if len(data) != size:
            raise BootstrapError("esptool returned a short flash read")
        return data

    def write(self, address: int, data: bytes, name: str) -> None:
        allowed = (
            address == OTA_0_OFFSET and 0 < len(data) <= OTA_SLOT_SIZE,
            address in (OTADATA_OFFSET, OTADATA_OFFSET + FLASH_SECTOR_SIZE)
            and len(data) == _OTA_RECORD.size,
        )
        if not any(allowed):
            raise BootstrapError("refusing flash write outside ota_0 or one OTA record")
        try:
            from esptool.cmds import write_flash
        except ImportError as error:
            raise BootstrapError("esptool write support is unavailable") from error
        source = _NamedBytesIO(data, name)
        args = SimpleNamespace(
            addr_filename=[(address, source)],
            compress=True,
            no_compress=False,
            no_stub=False,
            force=False,
            encrypt=False,
            encrypt_files=None,
            ignore_flash_encryption_efuse_setting=False,
            flash_size="keep",
            flash_mode="keep",
            flash_freq="keep",
            erase_all=False,
        )
        write_flash(self._esp, args)

    def erase(self, address: int, size: int) -> None:
        if address not in (OTADATA_OFFSET, OTADATA_OFFSET + FLASH_SECTOR_SIZE) or size != FLASH_SECTOR_SIZE:
            raise BootstrapError("refusing erase outside one OTA metadata sector")
        self._esp.erase_region(address, size)

    def hard_reset(self) -> None:
        self._esp.hard_reset()

    def close(self) -> None:
        esp = getattr(self, "_esp", None)
        port = getattr(esp, "_port", None)
        if port is not None:
            try:
                port.close()
            except Exception:
                pass


def validate_standard_esp_image(firmware: bytes) -> None:
    try:
        from esptool.bin_image import LoadFirmwareImage
    except ImportError as error:
        raise BootstrapError(
            "esptool is unavailable; source ESP-IDF 5.5.5 export.sh first"
        ) from error
    source = _NamedBytesIO(firmware, "firmware.bin")
    try:
        image = LoadFirmwareImage("ESP32-S3", source)
        image.verify()
    except Exception as error:
        raise BootstrapError(f"firmware is not a valid ESP32-S3 image: {error}") from error
    if image.chip_id != ESP32S3_IMAGE_CHIP_ID:
        raise BootstrapError("firmware image does not target ESP32-S3")


def perform_bootstrap(
    session: RomSession,
    evidence: RunningEvidence,
    package: BootstrapPackage,
    reviewed_partition_table: bytes,
) -> BootstrapPlan:
    """Run the commit-last flash transaction; caller owns reset and health proof."""

    live_partition_table = session.read(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE)
    verify_partition_table(live_partition_table, reviewed_partition_table)
    nvs_before = session.read(NVS_OFFSET, NVS_SIZE)
    otadata_before = session.read(OTADATA_OFFSET, OTADATA_SIZE)
    running_prefix = session.read(OTA_1_OFFSET, APP_DESCRIPTOR_READ_SIZE)
    running_descriptor = parse_app_descriptor(running_prefix)
    plan = make_bootstrap_plan(evidence, otadata_before, running_descriptor, package)

    session.write(plan.target_app_offset, package.firmware, "signed-nightglass-0.2.4.bin")
    readback = session.read(plan.target_app_offset, package.image_size)
    if readback != package.firmware or hashlib.sha256(readback).digest() != package.image_sha256:
        raise PreservationError("ota_0 readback differs from the signed firmware")

    verify_unchanged(otadata_before, session.read(OTADATA_OFFSET, OTADATA_SIZE), "otadata before commit")
    verify_unchanged(nvs_before, session.read(NVS_OFFSET, NVS_SIZE), "NVS before commit")

    session.write(
        plan.target_metadata_offset,
        plan.metadata_record,
        "nightglass-ota-new-record.bin",
    )
    otadata_after = session.read(OTADATA_OFFSET, OTADATA_SIZE)
    try:
        verify_metadata_commit(otadata_before, otadata_after, plan)
        verify_unchanged(nvs_before, session.read(NVS_OFFSET, NVS_SIZE), "NVS after commit")
    except PreservationError:
        # Removing only the newly-written inactive record restores selection of
        # the untouched valid ota_1 record.  Never rewrite the active sector.
        session.erase(plan.target_metadata_offset, FLASH_SECTOR_SIZE)
        erased = session.read(plan.target_metadata_offset, FLASH_SECTOR_SIZE)
        if erased != b"\xff" * FLASH_SECTOR_SIZE:
            raise PreservationError(
                "metadata verification failed and inactive-sector recovery also failed"
            )
        raise
    return plan


class HealthLogTracker:
    """Bounded parser for the exact pending/acceptance proof after reset."""

    def __init__(self) -> None:
        self._log = bytearray()
        self.pending_boot_seen = False
        self.usb_boot_seen = False
        self.usb_ready_seen = False
        self.accepted = False

    def feed(self, data: bytes) -> None:
        self._log.extend(data)
        if len(self._log) > 1024 * 1024:
            del self._log[: len(self._log) - 1024 * 1024]
        log = bytes(self._log)
        if _OLD_SLOT_BOOT.search(log):
            raise BootstrapError("0.2.4 rolled back to the preserved ota_1 image")
        rollback = _ROLLBACK_MARKER.search(log)
        if rollback is not None:
            raise BootstrapError("0.2.4 requested rollback during its health gate")
        pending_boot = _EXPECTED_PENDING_BOOT.search(log)
        usb_boot = _EXPECTED_USB_BOOT.search(log)
        self.pending_boot_seen = pending_boot is not None
        self.usb_boot_seen = usb_boot is not None
        self.usb_ready_seen = _EXPECTED_USB_READY in log
        proof_end = max(pending_boot.end(), usb_boot.end()) if pending_boot and usb_boot else len(log)
        accepted_at = log.find(_HEALTH_ACCEPTED, proof_end)
        self.accepted = (
            self.pending_boot_seen
            and self.usb_boot_seen
            and self.usb_ready_seen
            and accepted_at >= 0
        )


def wait_for_health(
    path: Path,
    timeout: float,
    *,
    mirror_serial: bool,
) -> HealthLogTracker:
    deadline = time.monotonic() + timeout
    port: PosixSerialPort | None = None
    tracker = HealthLogTracker()
    next_probe = 0.0
    try:
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if port is None:
                try:
                    port = PosixSerialPort(path)
                except (OSError, DevicePathError):
                    time.sleep(min(0.1, max(remaining, 0.0)))
                    continue
            try:
                # The running firmware emits USB_UPDATE_BOOT in response to a
                # status query. Passive logging alone can never prove that the
                # newly installed USB receiver actually handles commands.
                now = time.monotonic()
                if tracker.usb_ready_seen and now >= next_probe:
                    port.write_all(encode_envelope(build_control_payload(UPDATE_STATUS_QUERY, 1)),
                                   min(1.0, remaining))
                    next_probe = now + 2.0
                data = port.read(min(1.0, remaining))
            except AckTimeout:
                continue
            except LinkDisconnected:
                port.close()
                port = None
                continue
            if mirror_serial:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
            tracker.feed(data)
            if tracker.accepted:
                return tracker
    finally:
        if port is not None:
            port.close()
    missing = []
    if not tracker.pending_boot_seen:
        missing.append("pending ota_0 boot")
    if not tracker.usb_boot_seen:
        missing.append("0.2.4 USB pending marker")
    if not tracker.usb_ready_seen:
        missing.append("USB_UPDATE_READY")
    if not tracker.accepted:
        missing.append("OTA_HEALTH_ACCEPTED")
    raise BootstrapError("timed out waiting for " + ", ".join(missing))


def _positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def _baud(value: str) -> int:
    parsed = int(value)
    if parsed not in (115200, 230400, 460800, 921600):
        raise argparse.ArgumentTypeError("must be 115200, 230400, 460800, or 921600")
    return parsed


def _parser() -> argparse.ArgumentParser:
    project = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description=(
            "One-time direct ROM bootstrap from valid ota_1 Nightglass 0.2.2 "
            "to signed ota_0 Nightglass 0.2.4"
        )
    )
    parser.add_argument("package", type=Path)
    parser.add_argument(
        "--device",
        required=True,
        type=Path,
        help="exact /dev/serial/by-id/usb-Espressif_USB_JTAG...-if00 path",
    )
    evidence = parser.add_mutually_exclusive_group(required=True)
    evidence.add_argument(
        "--running-evidence",
        help="fresh, exact OTA_BOOT line from the current uninterrupted boot",
    )
    evidence.add_argument(
        "--running-evidence-file",
        type=Path,
        help="file containing the fresh OTA_BOOT line from the current boot",
    )
    parser.add_argument("--public-key", type=Path)
    parser.add_argument(
        "--partition-table",
        type=Path,
        default=project / "build" / "partition_table" / "partition-table.bin",
    )
    parser.add_argument("--baud", type=_baud, default=460800)
    parser.add_argument("--health-timeout", type=_positive_float, default=150.0)
    parser.add_argument(
        "--quiet-serial", action="store_true", help="do not mirror post-reset watch logs"
    )
    return parser


def _read_evidence(args: argparse.Namespace) -> str:
    if args.running_evidence is not None:
        return args.running_evidence
    return args.running_evidence_file.read_text(encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    session: EsptoolRomSession | None = None
    reset_attempted = False
    try:
        evidence = parse_running_evidence(_read_evidence(args))
        signed = load_signed_package(args.package, args.public_key)
        package = freeze_signed_package(signed)
        validate_standard_esp_image(package.firmware)
        reviewed_partition_table = args.partition_table.read_bytes()

        project = Path(__file__).resolve().parent.parent
        subprocess.run(
            [
                sys.executable,
                os.fspath(project / "scripts" / "verify-partitions.py"),
                os.fspath(project / "partitions.csv"),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        device = validate_bootstrap_device_path(args.device)
        print(
            f"Verified signed Nightglass 0.2.4 package ({package.image_size} bytes); "
            "starting one-time ota_0 bootstrap",
            file=sys.stderr,
        )

        session = EsptoolRomSession(device, args.baud)
        plan = perform_bootstrap(session, evidence, package, reviewed_partition_table)
        print(
            f"Committed ota_0 as NEW with sequence {plan.new_sequence}; resetting once",
            file=sys.stderr,
        )
        session.hard_reset()
        reset_attempted = True
        session.close()
        session = None
        wait_for_health(
            device,
            args.health_timeout,
            mirror_serial=not args.quiet_serial,
        )
        print(
            "Direct bootstrap accepted: ota_0 is VALID, rollback was preserved, "
            "and native signed USB updates are ready",
            file=sys.stderr,
        )
        return 0
    except KeyboardInterrupt:
        print("Nightglass ROM bootstrap interrupted", file=sys.stderr)
        return 130
    except (BootstrapError, DevicePathError, OSError, PackageError, subprocess.CalledProcessError) as error:
        print(f"Nightglass ROM bootstrap failed safely: {error}", file=sys.stderr)
        return 1
    except Exception as error:
        print(f"Nightglass ROM bootstrap failed safely: {error}", file=sys.stderr)
        return 1
    finally:
        if session is not None:
            if not reset_attempted:
                try:
                    session.hard_reset()
                except Exception:
                    pass
            session.close()


if __name__ == "__main__":
    raise SystemExit(main())
