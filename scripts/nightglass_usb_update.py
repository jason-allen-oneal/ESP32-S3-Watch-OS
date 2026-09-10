#!/usr/bin/env python3
"""Signed Nightglass application updates over the native USB serial link.

The USB wire envelope is ``NGU1``, a little-endian uint16 payload length, the
existing Nightglass update payload, and a little-endian IEEE/zlib CRC32 of the
payload.  This module deliberately uses ``os.open`` and ``termios`` directly;
it never toggles DTR or invokes a ROM bootloader/flashing tool.
"""

from __future__ import annotations

import argparse
from collections import deque
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import stat
import struct
import subprocess
import sys
import termios
import time
from dataclasses import dataclass
from typing import Callable, Protocol
import zlib


USB_MAGIC = b"NGU1"
PROTOCOL_VERSION = 1
UPDATE_BEGIN = 0x30
UPDATE_DATA = 0x31
UPDATE_FINISH = 0x32
UPDATE_ABORT = 0x33
UPDATE_STATUS_QUERY = 0x34
UPDATE_STATUS = 0x35

UPDATE_STATE_DISABLED = 0
UPDATE_STATE_IDLE = 1
UPDATE_STATE_RECEIVING = 2
UPDATE_STATE_READY_TO_REBOOT = 3
UPDATE_STATE_FAILED = 4
SIGNATURE_VERIFIED = 1
RESULT_PHYSICAL_CONFIRMATION_REQUIRED = 9

# Keep the update payload byte-identical to the proven BLE protocol.  The USB
# envelope improves transport reliability without growing the firmware's
# stack-resident UpdateTransportCommand.
MAX_USB_DATA_CHUNK = 230
MAX_USB_PAYLOAD = 244
MAX_IMAGE_SIZE = 6 * 1024 * 1024
EXPECTED_PACKAGE_FILES = frozenset(
    {"firmware.bin", "manifest.json", "manifest.payload", "manifest.sig"}
)
EXPECTED_BOARD = "nightglass-waveshare-esp32s3r8-2.06-v1.0"
EXPECTED_PARTITION = "nightglass-32m-r1"
STABLE_SERIAL_DIRECTORY = Path("/dev/serial/by-id")

_ENVELOPE_HEADER = struct.Struct("<4sH")
_ENVELOPE_CRC = struct.Struct("<I")
_STATUS = struct.Struct("<BBQBBBBII")
_HEALTH_ACCEPTED = b"OTA_HEALTH_ACCEPTED state=VALID pending=0"
_ROLLBACK = re.compile(
    rb"pending image failed health gate|requesting rollback", re.IGNORECASE
)


class UsbUpdateError(RuntimeError):
    """Base error for a safe, host-side update refusal."""


class PackageError(UsbUpdateError):
    """The four-file signed package did not pass validation."""


class ProtocolError(UsbUpdateError):
    """The watch sent an invalid or contradictory protocol response."""


class DevicePathError(UsbUpdateError):
    """The requested serial path is not a stable by-id character device."""


class LinkDisconnected(UsbUpdateError):
    """The USB serial device disconnected."""


class AckTimeout(UsbUpdateError):
    """A command was not acknowledged within its bounded timeout."""


@dataclass(frozen=True)
class SignedPackage:
    directory: Path
    firmware_path: Path
    canonical_payload: bytes
    signature: bytes
    format_version: int
    board_id: str
    partition_id: str
    partition_revision: int
    app_version: str
    secure_version: int
    image_size: int
    image_sha256: bytes


@dataclass(frozen=True)
class UpdateStatus:
    session: int
    state: int
    signature_state: int
    result: int
    expected_bytes: int
    received_bytes: int
    acknowledged_opcode: int = 0


@dataclass(frozen=True)
class EnvelopeEvent:
    payload: bytes


@dataclass(frozen=True)
class LogEvent:
    data: bytes


@dataclass(frozen=True)
class CorruptEnvelopeEvent:
    detail: str


StreamEvent = EnvelopeEvent | LogEvent | CorruptEnvelopeEvent


@dataclass(frozen=True)
class HealthEvidence:
    pending_boot: bytes
    accepted_marker: bytes


@dataclass(frozen=True)
class TransferProgress:
    sent_bytes: int
    total_bytes: int
    detail: str

    @property
    def percent(self) -> int:
        if self.total_bytes <= 0:
            return 0
        return min(100, (self.sent_bytes * 100) // self.total_bytes)


Verifier = Callable[[Path, Path | None], None]
LogSink = Callable[[bytes], None]
ProgressSink = Callable[[TransferProgress], None]


def _uint(manifest: dict[str, object], key: str, minimum: int, maximum: int) -> int:
    value = manifest.get(key)
    if type(value) is not int or not minimum <= value <= maximum:
        raise PackageError(f"manifest {key} is invalid")
    return value


def _canonical_text(manifest: dict[str, object], key: str, maximum: int) -> str:
    value = manifest.get(key)
    if not isinstance(value, str):
        raise PackageError(f"manifest {key} is invalid")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise PackageError(f"manifest {key} is not ASCII") from error
    if not 1 <= len(encoded) <= maximum or any(
        byte < 0x21 or byte > 0x7E or byte in (ord("\\"), ord("="))
        for byte in encoded
    ):
        raise PackageError(f"manifest {key} is not canonical")
    return value


def verify_signed_package(
    package_directory: Path,
    public_key: Path | None = None,
    *,
    verifier_script: Path | None = None,
    runner: Callable[..., subprocess.CompletedProcess[str]] | None = None,
) -> None:
    """Run the repository's existing independent four-file verifier."""

    script = verifier_script or Path(__file__).resolve().with_name(
        "verify-signed-package.py"
    )
    command = [sys.executable, os.fspath(script), os.fspath(package_directory)]
    if public_key is not None:
        command.extend(("--public-key", os.fspath(public_key)))
    execute = runner or subprocess.run
    completed = execute(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "verification failed").strip()
        raise PackageError(f"signed package verification failed: {detail}")


def load_signed_package(
    package_directory: Path,
    public_key: Path | None = None,
    *,
    verifier: Verifier | None = None,
) -> SignedPackage:
    """Verify and load an exact Nightglass four-file release package."""

    directory = Path(package_directory).resolve(strict=True)
    if not directory.is_dir():
        raise PackageError(f"package is not a directory: {directory}")
    entries = {entry.name: entry for entry in directory.iterdir()}
    names = frozenset(entries)
    if names != EXPECTED_PACKAGE_FILES:
        missing = sorted(EXPECTED_PACKAGE_FILES - names)
        unexpected = sorted(names - EXPECTED_PACKAGE_FILES)
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if unexpected:
            details.append("unexpected " + ", ".join(unexpected))
        raise PackageError("package must contain exactly four files (" + "; ".join(details) + ")")
    for name, entry in entries.items():
        if entry.is_symlink() or not entry.is_file():
            raise PackageError(f"package member must be a regular file: {name}")

    (verifier or verify_signed_package)(directory, public_key)

    try:
        raw_manifest = entries["manifest.json"].read_bytes()
        if not 2 <= len(raw_manifest) <= 8192:
            raise PackageError("manifest.json size is invalid")
        manifest = json.loads(raw_manifest.decode("utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PackageError(f"could not parse manifest.json: {error}") from error
    if not isinstance(manifest, dict):
        raise PackageError("manifest root must be an object")

    format_version = _uint(manifest, "format_version", 1, 0xFFFFFFFF)
    partition_revision = _uint(manifest, "partition_revision", 1, 0xFFFFFFFF)
    secure_version = _uint(manifest, "secure_version", 0, 0xFFFFFFFF)
    image_size = _uint(manifest, "image_size", 1, MAX_IMAGE_SIZE)
    board_id = _canonical_text(manifest, "board_id", 47)
    partition_id = _canonical_text(manifest, "partition_id", 31)
    app_version = _canonical_text(manifest, "app_version", 31)
    if format_version != 1 or partition_revision != 1:
        raise PackageError("package manifest revision is unsupported")
    if board_id != EXPECTED_BOARD or partition_id != EXPECTED_PARTITION:
        raise PackageError("package targets another watch or partition map")

    digest_text = manifest.get("image_sha256")
    if not isinstance(digest_text, str) or len(digest_text) != 64 or any(
        character not in "0123456789abcdef" for character in digest_text
    ):
        raise PackageError("manifest image_sha256 is not canonical lowercase hex")
    image_sha256 = bytes.fromhex(digest_text)
    signature_metadata = manifest.get("signature")
    if not isinstance(signature_metadata, dict) or (
        signature_metadata.get("algorithm") != "ecdsa-p256-sha256-der"
        or signature_metadata.get("file") != "manifest.sig"
    ):
        raise PackageError("manifest signature metadata is invalid")

    canonical_payload = entries["manifest.payload"].read_bytes()
    signature = entries["manifest.sig"].read_bytes()
    if len(canonical_payload) > 512:
        raise PackageError("manifest.payload exceeds its bounded size")
    if not 8 <= len(signature) <= 72 or signature[0] != 0x30:
        raise PackageError("manifest.sig is not bounded P-256 DER")
    if signature[1] >= 0x80 or signature[1] != len(signature) - 2:
        raise PackageError("manifest.sig has an invalid DER envelope")

    firmware_path = entries["firmware.bin"]
    firmware_stat = firmware_path.stat()
    if firmware_stat.st_size != image_size:
        raise PackageError("firmware size changed after signature verification")
    digest = hashlib.sha256()
    with firmware_path.open("rb") as firmware:
        for chunk in iter(lambda: firmware.read(64 * 1024), b""):
            digest.update(chunk)
    if digest.digest() != image_sha256:
        raise PackageError("firmware digest changed after signature verification")

    return SignedPackage(
        directory=directory,
        firmware_path=firmware_path,
        canonical_payload=canonical_payload,
        signature=signature,
        format_version=format_version,
        board_id=board_id,
        partition_id=partition_id,
        partition_revision=partition_revision,
        app_version=app_version,
        secure_version=secure_version,
        image_size=image_size,
        image_sha256=image_sha256,
    )


def derive_session(package: SignedPackage) -> int:
    """Derive a repeatable correlation id so rerunning the same package resumes."""

    digest = hashlib.sha256(b"Nightglass USB update session v1\0")
    digest.update(package.canonical_payload)
    session = int.from_bytes(digest.digest()[:8], "little")
    return session or 1


def build_begin_payload(package: SignedPackage, session: int) -> bytes:
    _require_session(session)
    board = package.board_id.encode("ascii")
    partition = package.partition_id.encode("ascii")
    version = package.app_version.encode("ascii")
    signature = package.signature
    payload = struct.pack(
        "<BBQIIII32sBBBB",
        PROTOCOL_VERSION,
        UPDATE_BEGIN,
        session,
        package.format_version,
        package.partition_revision,
        package.secure_version,
        package.image_size,
        package.image_sha256,
        len(board),
        len(partition),
        len(version),
        len(signature),
    ) + board + partition + version + signature
    if len(payload) > 244:
        raise PackageError("manifest cannot fit the signed update begin frame")
    return payload


def build_data_payload(session: int, offset: int, data: bytes) -> bytes:
    _require_session(session)
    if not 0 <= offset <= 0xFFFFFFFF:
        raise ValueError("update offset is outside uint32")
    if not 1 <= len(data) <= MAX_USB_DATA_CHUNK:
        raise ValueError(f"USB update chunks must be 1..{MAX_USB_DATA_CHUNK} bytes")
    return struct.pack("<BBQI", PROTOCOL_VERSION, UPDATE_DATA, session, offset) + data


def build_control_payload(opcode: int, session: int) -> bytes:
    _require_session(session)
    if opcode not in (UPDATE_FINISH, UPDATE_ABORT, UPDATE_STATUS_QUERY):
        raise ValueError("invalid update control opcode")
    return struct.pack("<BBQ", PROTOCOL_VERSION, opcode, session)


def _require_session(session: int) -> None:
    if not 1 <= session <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("update session must be a nonzero uint64")


def parse_status_payload(payload: bytes) -> UpdateStatus:
    if len(payload) != _STATUS.size:
        raise ProtocolError(f"update status length is {len(payload)}, expected 22")
    version, opcode, session, state, signature, result, reserved, expected, received = (
        _STATUS.unpack(payload)
    )
    if version != PROTOCOL_VERSION or opcode != UPDATE_STATUS or reserved not in (
        0,
        UPDATE_BEGIN,
        UPDATE_DATA,
        UPDATE_FINISH,
        UPDATE_ABORT,
        UPDATE_STATUS_QUERY,
    ):
        raise ProtocolError("update status header is invalid")
    if session == 0 or state > UPDATE_STATE_FAILED or signature > 4 or result > 9:
        raise ProtocolError("update status fields are outside protocol bounds")
    if received > expected:
        raise ProtocolError("watch reports more update bytes than expected")
    return UpdateStatus(
        session, state, signature, result, expected, received, reserved
    )


def encode_envelope(payload: bytes) -> bytes:
    if not 1 <= len(payload) <= MAX_USB_PAYLOAD:
        raise ValueError(f"USB payload must be 1..{MAX_USB_PAYLOAD} bytes")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return _ENVELOPE_HEADER.pack(USB_MAGIC, len(payload)) + payload + _ENVELOPE_CRC.pack(crc)


def decode_envelope(envelope: bytes) -> bytes:
    if len(envelope) < _ENVELOPE_HEADER.size + _ENVELOPE_CRC.size:
        raise ProtocolError("USB envelope is truncated")
    magic, payload_size = _ENVELOPE_HEADER.unpack_from(envelope)
    expected_size = _ENVELOPE_HEADER.size + payload_size + _ENVELOPE_CRC.size
    if magic != USB_MAGIC or not 1 <= payload_size <= MAX_USB_PAYLOAD:
        raise ProtocolError("USB envelope header is invalid")
    if len(envelope) != expected_size:
        raise ProtocolError("USB envelope length does not match its header")
    payload = envelope[_ENVELOPE_HEADER.size : -_ENVELOPE_CRC.size]
    supplied_crc = _ENVELOPE_CRC.unpack_from(envelope, len(envelope) - 4)[0]
    computed_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if supplied_crc != computed_crc:
        raise ProtocolError("USB envelope CRC32 mismatch")
    return payload


class EnvelopeStreamDecoder:
    """Split framed binary replies from ordinary ESP-IDF serial log bytes."""

    def __init__(self) -> None:
        self._buffer = bytearray()

    def reset(self) -> None:
        self._buffer.clear()

    @staticmethod
    def _partial_magic_suffix(data: bytearray) -> int:
        for size in range(min(len(USB_MAGIC) - 1, len(data)), 0, -1):
            if data[-size:] == USB_MAGIC[:size]:
                return size
        return 0

    def feed(self, data: bytes) -> list[StreamEvent]:
        self._buffer.extend(data)
        events: list[StreamEvent] = []
        while self._buffer:
            magic_at = self._buffer.find(USB_MAGIC)
            if magic_at < 0:
                retained = self._partial_magic_suffix(self._buffer)
                emitted = len(self._buffer) - retained
                if emitted:
                    events.append(LogEvent(bytes(self._buffer[:emitted])))
                    del self._buffer[:emitted]
                break
            if magic_at:
                events.append(LogEvent(bytes(self._buffer[:magic_at])))
                del self._buffer[:magic_at]
            if len(self._buffer) < _ENVELOPE_HEADER.size:
                break
            _, payload_size = _ENVELOPE_HEADER.unpack_from(self._buffer)
            if not 1 <= payload_size <= MAX_USB_PAYLOAD:
                events.append(CorruptEnvelopeEvent("USB envelope payload length is invalid"))
                del self._buffer[: len(USB_MAGIC)]
                continue
            envelope_size = _ENVELOPE_HEADER.size + payload_size + _ENVELOPE_CRC.size
            if len(self._buffer) < envelope_size:
                break
            envelope = bytes(self._buffer[:envelope_size])
            del self._buffer[:envelope_size]
            try:
                events.append(EnvelopeEvent(decode_envelope(envelope)))
            except ProtocolError as error:
                events.append(CorruptEnvelopeEvent(str(error)))
        return events


def validate_stable_serial_path(path: Path) -> Path:
    """Require a persistent Linux by-id symlink resolving to a character device."""

    candidate = Path(path)
    if not candidate.is_absolute() or candidate.parent != STABLE_SERIAL_DIRECTORY:
        raise DevicePathError(
            "device must be an exact /dev/serial/by-id/<device> path"
        )
    if not candidate.name or not candidate.is_symlink():
        raise DevicePathError("device must be an existing /dev/serial/by-id symlink")
    try:
        candidate.resolve(strict=True)
        mode = os.stat(candidate).st_mode
    except OSError as error:
        raise DevicePathError(f"serial device is unavailable: {candidate}") from error
    if not stat.S_ISCHR(mode):
        raise DevicePathError("stable serial path does not resolve to a character device")
    return candidate


def competing_device_holders(path: Path) -> list[int]:
    """Return other process ids with this character device already open."""

    try:
        device = os.stat(path)
    except OSError as error:
        raise DevicePathError(f"serial device is unavailable: {path}") from error
    holders: list[int] = []
    current_pid = os.getpid()
    try:
        processes = Path("/proc").iterdir()
    except OSError:
        return holders
    for process in processes:
        if not process.name.isdigit() or int(process.name) == current_pid:
            continue
        try:
            descriptors = list((process / "fd").iterdir())
        except (OSError, PermissionError):
            continue
        for descriptor in descriptors:
            try:
                opened = descriptor.stat()
            except (OSError, PermissionError):
                continue
            if stat.S_ISCHR(opened.st_mode) and opened.st_rdev == device.st_rdev:
                holders.append(int(process.name))
                break
    return sorted(set(holders))


class PosixSerialPort:
    """Nonblocking serial byte transport that never manipulates modem lines."""

    def __init__(self, path: Path, baud: int = 115200) -> None:
        if baud != 115200:
            raise ValueError("Nightglass USB serial transport uses 115200 baud")
        self.path = Path(path)
        self.baud = baud
        self.fd: int | None = None
        self._open_once()

    def _open_once(self) -> None:
        path = validate_stable_serial_path(self.path)
        holders = competing_device_holders(path)
        if holders:
            raise DevicePathError(
                "serial device is already open by process id(s) "
                + ", ".join(str(pid) for pid in holders)
                + "; close serial monitors before installing"
            )
        flags = os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        fd = os.open(os.fspath(path), flags)
        try:
            fcntl.ioctl(fd, termios.TIOCEXCL)
            raced_holders = competing_device_holders(path)
            if raced_holders:
                raise DevicePathError(
                    "serial device was opened concurrently by process id(s) "
                    + ", ".join(str(pid) for pid in raced_holders)
                )
            attributes = termios.tcgetattr(fd)
            attributes[0] = 0
            attributes[1] = 0
            attributes[2] &= ~(
                termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.HUPCL
            )
            if hasattr(termios, "CRTSCTS"):
                attributes[2] &= ~termios.CRTSCTS
            attributes[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
            attributes[3] = 0
            attributes[4] = termios.B115200
            attributes[5] = termios.B115200
            attributes[6][termios.VMIN] = 0
            attributes[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, attributes)
        except Exception:
            os.close(fd)
            raise
        self.fd = fd

    def close(self) -> None:
        fd, self.fd = self.fd, None
        if fd is not None:
            os.close(fd)

    def reconnect(self, timeout: float) -> None:
        self.close()
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while True:
            try:
                self._open_once()
                return
            except (OSError, DevicePathError, termios.error) as error:
                last_error = error
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise LinkDisconnected(
                    f"stable serial device did not return: {self.path}"
                ) from last_error
            time.sleep(min(0.1, remaining))

    def write_all(self, data: bytes, timeout: float) -> None:
        if self.fd is None:
            raise LinkDisconnected("USB serial link is closed")
        deadline = time.monotonic() + timeout
        view = memoryview(data)
        while view:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AckTimeout("USB serial write timed out")
            try:
                written = os.write(self.fd, view)
            except BlockingIOError:
                written = 0
            except OSError as error:
                raise LinkDisconnected("USB serial write failed") from error
            if written:
                view = view[written:]
                continue
            try:
                _, writable, _ = select.select([], [self.fd], [], remaining)
            except OSError as error:
                raise LinkDisconnected("USB serial link disappeared") from error
            if not writable:
                raise AckTimeout("USB serial write timed out")

    def read(self, timeout: float) -> bytes:
        if self.fd is None:
            raise LinkDisconnected("USB serial link is closed")
        try:
            readable, _, _ = select.select([self.fd], [], [], timeout)
        except OSError as error:
            raise LinkDisconnected("USB serial link disappeared") from error
        if not readable:
            raise AckTimeout("USB serial read timed out")
        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError as error:
            raise AckTimeout("USB serial read timed out") from error
        except OSError as error:
            raise LinkDisconnected("USB serial read failed") from error
        if not data:
            raise LinkDisconnected("USB serial device disconnected")
        return data

    def __enter__(self) -> PosixSerialPort:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class UpdateLink(Protocol):
    def send(self, payload: bytes, timeout: float) -> None: ...

    def receive_status(self, expected_session: int, timeout: float) -> UpdateStatus: ...

    def exchange(
        self,
        payload: bytes,
        expected_session: int,
        timeout: float,
        *,
        accept_foreign_session: bool = False,
    ) -> UpdateStatus: ...

    def reconnect(self, timeout: float) -> None: ...

    def clear_log_history(self) -> None: ...

    def wait_for_reboot_health(
        self, session: int, app_version: str, secure_version: int, timeout: float
    ) -> HealthEvidence: ...


class FramedUsbLink:
    """One-in-flight-command framed USB link with status acknowledgements."""

    def __init__(self, port: PosixSerialPort, log_sink: LogSink | None = None) -> None:
        self.port = port
        self.decoder = EnvelopeStreamDecoder()
        self.log_sink = log_sink
        self._log_history = bytearray()
        self._maximum_log_history = 1024 * 1024
        self._pending_payloads: deque[bytes] = deque()

    def close(self) -> None:
        self.port.close()

    def reconnect(self, timeout: float) -> None:
        self.port.reconnect(timeout)
        self.decoder.reset()
        self._pending_payloads.clear()

    def clear_log_history(self) -> None:
        self._log_history.clear()

    def _record_log(self, data: bytes) -> None:
        self._log_history.extend(data)
        overflow = len(self._log_history) - self._maximum_log_history
        if overflow > 0:
            del self._log_history[:overflow]
        if self.log_sink is not None:
            self.log_sink(data)

    def _read_events(self, timeout: float) -> list[StreamEvent]:
        events = self.decoder.feed(self.port.read(timeout))
        for event in events:
            if isinstance(event, LogEvent):
                self._record_log(event.data)
            elif isinstance(event, CorruptEnvelopeEvent):
                raise ProtocolError(event.detail)
            elif isinstance(event, EnvelopeEvent):
                self._pending_payloads.append(event.payload)
        return events

    def send(self, payload: bytes, timeout: float) -> None:
        self.port.write_all(encode_envelope(payload), timeout)

    def receive_status(self, expected_session: int, timeout: float) -> UpdateStatus:
        deadline = time.monotonic() + timeout
        while True:
            while self._pending_payloads:
                status = parse_status_payload(self._pending_payloads.popleft())
                if status.session != expected_session:
                    raise ProtocolError(
                        f"watch acknowledged unexpected session 0x{status.session:016x}"
                    )
                if status.acknowledged_opcode == 0:
                    raise ProtocolError("watch USB status lacks acknowledged opcode")
                if status.acknowledged_opcode != UPDATE_DATA:
                    continue
                return status
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AckTimeout("watch update acknowledgement timed out")
            self._read_events(remaining)

    def exchange(
        self,
        payload: bytes,
        expected_session: int,
        timeout: float,
        *,
        accept_foreign_session: bool = False,
    ) -> UpdateStatus:
        self.send(payload, timeout)
        deadline = time.monotonic() + timeout
        while True:
            while self._pending_payloads:
                status = parse_status_payload(self._pending_payloads.popleft())
                if status.session != expected_session and not accept_foreign_session:
                    raise ProtocolError(
                        f"watch acknowledged unexpected session 0x{status.session:016x}"
                    )
                if status.acknowledged_opcode == 0:
                    raise ProtocolError("watch USB status lacks acknowledged opcode")
                if status.acknowledged_opcode != payload[1]:
                    continue
                return status
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AckTimeout("watch update acknowledgement timed out")
            self._read_events(remaining)

    def _health_evidence(
        self, app_version: str, secure_version: int
    ) -> HealthEvidence | None:
        log = bytes(self._log_history)
        exact_pending = re.compile(
            rb"USB_UPDATE_BOOT[^\r\n]*\bversion="
            + re.escape(app_version.encode("ascii"))
            + rb"\b[^\r\n]*\bsecure="
            + str(secure_version).encode("ascii")
            + rb"\b[^\r\n]*\bpending=1\b"
        )
        pending = exact_pending.search(log)
        if pending is None:
            return None
        rollback = _ROLLBACK.search(log, pending.end())
        accepted_at = log.find(_HEALTH_ACCEPTED, pending.end())
        if rollback is not None and (accepted_at < 0 or rollback.start() < accepted_at):
            raise ProtocolError("updated image requested rollback during its health gate")
        if accepted_at < 0:
            return None
        return HealthEvidence(pending.group(0), _HEALTH_ACCEPTED)

    def wait_for_reboot_health(
        self, session: int, app_version: str, secure_version: int, timeout: float
    ) -> HealthEvidence:
        deadline = time.monotonic() + timeout
        next_probe = 0.0
        while True:
            evidence = self._health_evidence(app_version, secure_version)
            if evidence is not None:
                return evidence
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AckTimeout(
                    "timed out waiting for the exact pending image and "
                    "OTA_HEALTH_ACCEPTED"
                )
            now = time.monotonic()
            if now >= next_probe:
                try:
                    self.send(
                        build_control_payload(UPDATE_STATUS_QUERY, session),
                        min(1.0, remaining),
                    )
                    next_probe = now + 1.0
                except AckTimeout:
                    next_probe = now + 0.25
                except LinkDisconnected:
                    try:
                        self.reconnect(min(remaining, 15.0))
                        next_probe = 0.0
                        continue
                    except LinkDisconnected:
                        continue
            try:
                self._read_events(min(1.0, remaining))
                while self._pending_payloads:
                    status = parse_status_payload(self._pending_payloads.popleft())
                    if status.session != session:
                        raise ProtocolError(
                            "post-reboot watch reported another update session"
                        )
            except AckTimeout:
                continue
            except LinkDisconnected:
                try:
                    self.reconnect(min(remaining, 15.0))
                    next_probe = 0.0
                except LinkDisconnected:
                    # Keep the overall health deadline authoritative; a slow USB
                    # re-enumeration must not manufacture an earlier deadline.
                    continue


class UsbUpdateInstaller:
    """Stop-and-wait signed update transfer with status-based resumption."""

    def __init__(
        self,
        link: UpdateLink,
        package: SignedPackage,
        *,
        chunk_size: int = MAX_USB_DATA_CHUNK,
        ack_timeout: float = 10.0,
        reconnect_timeout: float = 30.0,
        health_timeout: float = 120.0,
        progress_sink: ProgressSink | None = None,
    ) -> None:
        if not 1 <= chunk_size <= MAX_USB_DATA_CHUNK:
            raise ValueError(f"chunk_size must be 1..{MAX_USB_DATA_CHUNK}")
        if ack_timeout <= 0 or reconnect_timeout <= 0 or health_timeout <= 0:
            raise ValueError("all installer timeouts must be positive")
        self.link = link
        self.package = package
        self.chunk_size = chunk_size
        self.ack_timeout = ack_timeout
        self.reconnect_timeout = reconnect_timeout
        self.health_timeout = health_timeout
        self.progress_sink = progress_sink

    def _progress(self, sent: int, detail: str) -> None:
        if self.progress_sink is not None:
            self.progress_sink(TransferProgress(sent, self.package.image_size, detail))

    @staticmethod
    def _require_success(status: UpdateStatus, operation: str) -> None:
        if status.result == RESULT_PHYSICAL_CONFIRMATION_REQUIRED:
            raise ProtocolError(
                "watch firmware still requires physical update confirmation"
            )
        if status.result != 0:
            names = {
                2: "degraded",
                3: "unavailable",
                4: "invalid state",
                5: "timeout",
                6: "I/O error",
                7: "out of memory",
            }
            detail = names.get(status.result, f"result {status.result}")
            raise ProtocolError(f"watch rejected {operation}: {detail}")

    def _query(self, session: int, *, accept_foreign: bool = False) -> UpdateStatus:
        deadline = time.monotonic() + self.reconnect_timeout
        last_error: Exception | None = None
        while True:
            try:
                return self.link.exchange(
                    build_control_payload(UPDATE_STATUS_QUERY, session),
                    session,
                    self.ack_timeout,
                    accept_foreign_session=accept_foreign,
                )
            except AckTimeout as error:
                last_error = error
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AckTimeout("could not obtain watch update status") from last_error
                # The link is still present. Send another STATUS query without
                # closing the tty (and therefore without disturbing modem lines).
                continue
            except LinkDisconnected as error:
                last_error = error
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AckTimeout("could not obtain watch update status") from last_error
                try:
                    self.link.reconnect(min(remaining, self.ack_timeout))
                except LinkDisconnected as reconnect_error:
                    last_error = reconnect_error

    def _verify_firmware_unchanged(self) -> None:
        try:
            if self.package.firmware_path.stat().st_size != self.package.image_size:
                raise PackageError("firmware changed after package verification")
            digest = hashlib.sha256()
            with self.package.firmware_path.open("rb") as firmware:
                for chunk in iter(lambda: firmware.read(64 * 1024), b""):
                    digest.update(chunk)
        except OSError as error:
            raise PackageError(f"could not re-read verified firmware: {error}") from error
        if digest.digest() != self.package.image_sha256:
            raise PackageError("firmware changed after package verification")

    def _clear_foreign_session(self, status: UpdateStatus, session: int) -> UpdateStatus:
        if status.session == session:
            return status
        self._progress(0, f"clearing prior update session 0x{status.session:016x}")
        try:
            acknowledged = self.link.exchange(
                build_control_payload(UPDATE_ABORT, status.session),
                status.session,
                self.ack_timeout,
            )
        except (AckTimeout, LinkDisconnected):
            acknowledged = self._query(status.session)
        self._require_success(acknowledged, "prior-session abort")
        if acknowledged.state != UPDATE_STATE_IDLE:
            raise ProtocolError("prior watch update session did not become idle")
        return self._query(session)

    def _begin(self, session: int) -> UpdateStatus:
        payload = build_begin_payload(self.package, session)
        for attempt in range(3):
            try:
                status = self.link.exchange(payload, session, self.ack_timeout)
            except (AckTimeout, LinkDisconnected):
                status = self._query(session)
            self._require_success(status, "signed update begin")
            if status.state == UPDATE_STATE_RECEIVING:
                if status.signature_state != SIGNATURE_VERIFIED:
                    raise ProtocolError("watch did not report a verified package signature")
                if (
                    status.expected_bytes != self.package.image_size
                    or status.received_bytes > self.package.image_size
                ):
                    raise ProtocolError("watch resume position contradicts signed manifest")
                return status
            if status.state not in (UPDATE_STATE_IDLE, UPDATE_STATE_FAILED) or attempt == 2:
                raise ProtocolError(
                    f"watch did not enter receiving state after begin (state={status.state})"
                )
        raise ProtocolError("watch did not accept signed update begin")

    def _validate_receiving_status(self, status: UpdateStatus) -> None:
        self._require_success(status, "firmware data")
        if (
            status.state != UPDATE_STATE_RECEIVING
            or status.signature_state != SIGNATURE_VERIFIED
            or status.expected_bytes != self.package.image_size
        ):
            raise ProtocolError("watch left the verified receiving state")

    def _send_window(
        self, session: int, offset: int, chunks: list[bytes]
    ) -> UpdateStatus:
        """Send at most three frames, then drain their ordered status ACKs."""

        if not 1 <= len(chunks) <= 3:
            raise ValueError("USB update window must contain one to three frames")
        boundaries = []
        cursor = offset
        try:
            for data in chunks:
                cursor += len(data)
                boundaries.append(cursor)
                self.link.send(
                    build_data_payload(session, cursor - len(data), data),
                    self.ack_timeout,
                )
            acknowledged = offset
            status: UpdateStatus | None = None
            while acknowledged < boundaries[-1]:
                status = self.link.receive_status(session, self.ack_timeout)
                self._validate_receiving_status(status)
                if (
                    status.received_bytes not in boundaries
                    or status.received_bytes <= acknowledged
                ):
                    raise ProtocolError(
                        "watch returned a non-monotonic or unaligned window offset "
                        f"({status.received_bytes})"
                    )
                acknowledged = status.received_bytes
            assert status is not None
            return status
        except (AckTimeout, LinkDisconnected):
            # Delivery is ambiguous. A status query is the only safe recovery:
            # never replay a frame until the watch has supplied its exact offset.
            status = self._query(session)
            self._validate_receiving_status(status)
            if status.received_bytes not in (offset, *boundaries):
                raise ProtocolError(
                    "watch resume offset is not a transmitted frame boundary "
                    f"({status.received_bytes})"
                )
            return status

    def _finish(self, session: int) -> None:
        payload = build_control_payload(UPDATE_FINISH, session)
        self.link.clear_log_history()
        for attempt in range(3):
            try:
                status = self.link.exchange(payload, session, self.ack_timeout)
            except (AckTimeout, LinkDisconnected):
                try:
                    status = self._query(session)
                except AckTimeout:
                    # A reboot can remove the old serial instance before the final
                    # status frame is observed. Health evidence remains mandatory.
                    return
            self._require_success(status, "signed update finish")
            if status.state in (UPDATE_STATE_READY_TO_REBOOT, UPDATE_STATE_IDLE):
                return
            if (
                status.state != UPDATE_STATE_RECEIVING
                or status.received_bytes != self.package.image_size
                or attempt == 2
            ):
                raise ProtocolError(
                    f"watch did not enter ready-to-reboot state (state={status.state})"
                )

    def install(self) -> HealthEvidence:
        self._verify_firmware_unchanged()
        session = derive_session(self.package)
        self._progress(0, f"probing watch session 0x{session:016x}")
        status = self._query(session, accept_foreign=True)
        self._require_success(status, "status probe")
        status = self._clear_foreign_session(status, session)
        self._require_success(status, "status probe")
        if status.state == UPDATE_STATE_DISABLED:
            raise ProtocolError("watch update backend is disabled")

        if status.state != UPDATE_STATE_READY_TO_REBOOT:
            self._progress(status.received_bytes, "authorizing signed package")
            status = self._begin(session)
            offset = status.received_bytes
            self._progress(offset, "resuming verified firmware transfer" if offset else "transferring verified firmware")
            with self.package.firmware_path.open("rb") as firmware:
                while offset < self.package.image_size:
                    firmware.seek(offset)
                    chunks: list[bytes] = []
                    window_bytes = 0
                    for _ in range(3):
                        remaining = self.package.image_size - offset - window_bytes
                        if remaining <= 0:
                            break
                        wanted = min(self.chunk_size, remaining)
                        data = firmware.read(wanted)
                        if len(data) != wanted:
                            raise PackageError("firmware ended during verified transfer")
                        chunks.append(data)
                        window_bytes += len(data)
                    status = self._send_window(session, offset, chunks)
                    offset = status.received_bytes
                    self._progress(offset, "transferring verified firmware")
            self._progress(self.package.image_size, "requesting verified-slot reboot")
            self._finish(session)

        self._progress(self.package.image_size, "waiting for rollback health gate")
        evidence = self.link.wait_for_reboot_health(
            session,
            self.package.app_version,
            self.package.secure_version,
            self.health_timeout,
        )
        self._progress(self.package.image_size, "updated image accepted")
        return evidence


def _positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Install a four-file signed Nightglass package over native USB"
    )
    parser.add_argument("package", type=Path)
    parser.add_argument(
        "--device",
        required=True,
        type=Path,
        help="stable /dev/serial/by-id/... Nightglass USB serial path",
    )
    parser.add_argument("--public-key", type=Path)
    parser.add_argument("--chunk-size", type=int, default=MAX_USB_DATA_CHUNK)
    parser.add_argument("--ack-timeout", type=_positive_float, default=10.0)
    parser.add_argument("--reconnect-timeout", type=_positive_float, default=30.0)
    parser.add_argument("--health-timeout", type=_positive_float, default=120.0)
    parser.add_argument(
        "--quiet-serial", action="store_true", help="do not mirror watch boot logs"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        package = load_signed_package(args.package, args.public_key)
        # Validate the stable target before presenting the package as installable.
        validate_stable_serial_path(args.device)
        print(
            f"Verified signed Nightglass {package.app_version} package "
            f"({package.image_size} bytes)",
            file=sys.stderr,
        )

        last_progress: tuple[int, str] | None = None

        def report(progress: TransferProgress) -> None:
            nonlocal last_progress
            current = (progress.percent, progress.detail)
            if current != last_progress:
                print(f"[{progress.percent:3d}%] {progress.detail}", file=sys.stderr)
                last_progress = current

        def serial_log(data: bytes) -> None:
            if args.quiet_serial:
                return
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

        port = PosixSerialPort(args.device)
        link = FramedUsbLink(port, serial_log)
        try:
            installer = UsbUpdateInstaller(
                link,
                package,
                chunk_size=args.chunk_size,
                ack_timeout=args.ack_timeout,
                reconnect_timeout=args.reconnect_timeout,
                health_timeout=args.health_timeout,
                progress_sink=report,
            )
            installer.install()
        finally:
            link.close()
        print(
            "Signed app update accepted after reboot health gate; NVS was not erased",
            file=sys.stderr,
        )
        return 0
    except (OSError, ValueError, UsbUpdateError) as error:
        print(f"Nightglass USB update failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Nightglass USB update interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
