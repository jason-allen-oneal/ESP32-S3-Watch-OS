#!/usr/bin/env python3
"""Host-only tests for the signed native-USB update client."""

from __future__ import annotations

from collections import deque
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib


PROJECT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT / "scripts"))
import nightglass_usb_update as ngu  # noqa: E402


SIGNATURE = b"\x30\x06\x02\x01\x01\x02\x01\x01"


def make_package(directory: Path, firmware: bytes = b"firmware-image") -> ngu.SignedPackage:
    firmware_path = directory / "firmware.bin"
    firmware_path.write_bytes(firmware)
    return ngu.SignedPackage(
        directory=directory,
        firmware_path=firmware_path,
        canonical_payload=b"canonical signed payload",
        signature=SIGNATURE,
        format_version=1,
        board_id=ngu.EXPECTED_BOARD,
        partition_id=ngu.EXPECTED_PARTITION,
        partition_revision=1,
        app_version="9.8.7",
        secure_version=12,
        image_size=len(firmware),
        image_sha256=hashlib.sha256(firmware).digest(),
    )


def status(
    session: int,
    state: int,
    *,
    received: int = 0,
    expected: int = 0,
    result: int = 0,
    signature: int = ngu.SIGNATURE_VERIFIED,
    acknowledged_opcode: int = ngu.UPDATE_DATA,
) -> ngu.UpdateStatus:
    return ngu.UpdateStatus(
        session,
        state,
        signature,
        result,
        expected,
        received,
        acknowledged_opcode,
    )


def status_bytes(value: ngu.UpdateStatus) -> bytes:
    return struct.pack(
        "<BBQBBBBII",
        ngu.PROTOCOL_VERSION,
        ngu.UPDATE_STATUS,
        value.session,
        value.state,
        value.signature_state,
        value.result,
        value.acknowledged_opcode,
        value.expected_bytes,
        value.received_bytes,
    )


class FramingTest(unittest.TestCase):
    def test_envelope_is_payload_crc32_with_little_endian_fields(self) -> None:
        payload = b"\x01\x34" + bytes(range(8))
        envelope = ngu.encode_envelope(payload)
        self.assertEqual(envelope[:4], b"NGU1")
        self.assertEqual(envelope[4:6], struct.pack("<H", len(payload)))
        self.assertEqual(envelope[-4:], struct.pack("<I", zlib.crc32(payload)))
        self.assertEqual(ngu.decode_envelope(envelope), payload)

    def test_crc_corruption_is_rejected(self) -> None:
        envelope = bytearray(ngu.encode_envelope(b"hello"))
        envelope[-1] ^= 0x80
        with self.assertRaisesRegex(ngu.ProtocolError, "CRC32"):
            ngu.decode_envelope(bytes(envelope))

    def test_stream_decoder_preserves_logs_and_partial_magic(self) -> None:
        payload = status_bytes(status(7, ngu.UPDATE_STATE_IDLE))
        envelope = ngu.encode_envelope(payload)
        decoder = ngu.EnvelopeStreamDecoder()
        first = decoder.feed(b"boot line\nNG")
        self.assertEqual(first, [ngu.LogEvent(b"boot line\n")])
        second = decoder.feed(envelope[2:] + b"tail")
        self.assertEqual(second[0], ngu.EnvelopeEvent(payload))
        self.assertEqual(second[1], ngu.LogEvent(b"tail"))

    def test_stream_decoder_reports_corrupt_frame(self) -> None:
        envelope = bytearray(ngu.encode_envelope(b"payload"))
        envelope[-2] ^= 1
        events = ngu.EnvelopeStreamDecoder().feed(bytes(envelope))
        self.assertIsInstance(events[0], ngu.CorruptEnvelopeEvent)


class PayloadTest(unittest.TestCase):
    def test_data_payload_keeps_proven_244_byte_protocol_bound(self) -> None:
        payload = ngu.build_data_payload(0x8877665544332211, 0x12345678, b"x" * 230)
        self.assertEqual(len(payload), 244)
        self.assertEqual(payload[:2], b"\x01\x31")
        self.assertEqual(payload[2:10], struct.pack("<Q", 0x8877665544332211))
        self.assertEqual(payload[10:14], struct.pack("<I", 0x12345678))
        with self.assertRaises(ValueError):
            ngu.build_data_payload(1, 0, b"x" * 231)

    def test_begin_payload_matches_existing_update_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            package = make_package(Path(temporary))
            payload = ngu.build_begin_payload(package, 0x1234)
        self.assertEqual(payload[:2], b"\x01\x30")
        self.assertEqual(struct.unpack_from("<Q", payload, 2)[0], 0x1234)
        self.assertEqual(struct.unpack_from("<I", payload, 10)[0], 1)
        self.assertEqual(struct.unpack_from("<I", payload, 22)[0], package.image_size)
        self.assertEqual(payload[26:58], package.image_sha256)
        self.assertEqual(payload[58:62], bytes((len(package.board_id), len(package.partition_id), len(package.app_version), len(SIGNATURE))))

    def test_status_parser_rejects_impossible_offset(self) -> None:
        impossible = status_bytes(
            status(2, ngu.UPDATE_STATE_RECEIVING, received=11, expected=10)
        )
        with self.assertRaisesRegex(ngu.ProtocolError, "more update bytes"):
            ngu.parse_status_payload(impossible)

    def test_status_parser_preserves_acknowledged_opcode(self) -> None:
        reply = status_bytes(
            status(
                2,
                ngu.UPDATE_STATE_IDLE,
                acknowledged_opcode=ngu.UPDATE_STATUS_QUERY,
            )
        )
        self.assertEqual(
            ngu.parse_status_payload(reply).acknowledged_opcode,
            ngu.UPDATE_STATUS_QUERY,
        )


class PackageTest(unittest.TestCase):
    @staticmethod
    def write_package(directory: Path, firmware: bytes = b"signed-image") -> None:
        digest = hashlib.sha256(firmware).hexdigest()
        manifest = {
            "app_version": "1.2.3",
            "board_id": ngu.EXPECTED_BOARD,
            "format_version": 1,
            "image_sha256": digest,
            "image_size": len(firmware),
            "partition_id": ngu.EXPECTED_PARTITION,
            "partition_revision": 1,
            "secure_version": 4,
            "signature": {
                "algorithm": "ecdsa-p256-sha256-der",
                "file": "manifest.sig",
            },
        }
        canonical = (
            "NIGHTGLASS-OTA-MANIFEST\n"
            "format=1\n"
            f"board={ngu.EXPECTED_BOARD}\n"
            f"partition={ngu.EXPECTED_PARTITION}\n"
            "partition_revision=1\n"
            "version=1.2.3\n"
            "secure_version=4\n"
            f"size={len(firmware)}\n"
            f"sha256={digest}\n"
        ).encode("ascii")
        (directory / "firmware.bin").write_bytes(firmware)
        (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        (directory / "manifest.payload").write_bytes(canonical)
        (directory / "manifest.sig").write_bytes(SIGNATURE)

    def test_loader_requires_verifier_and_exact_four_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.write_package(directory)
            calls: list[tuple[Path, Path | None]] = []
            package = ngu.load_signed_package(
                directory,
                Path("/public.key"),
                verifier=lambda path, key: calls.append((path, key)),
            )
            self.assertEqual(calls, [(directory.resolve(), Path("/public.key"))])
            self.assertEqual(package.image_size, len(b"signed-image"))
            (directory / "extra.txt").write_text("not part of package", encoding="utf-8")
            with self.assertRaisesRegex(ngu.PackageError, "exactly four"):
                ngu.load_signed_package(directory, verifier=lambda *_: None)

    def test_existing_verifier_is_invoked_without_shell(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, "verified", "")

        ngu.verify_signed_package(
            Path("/tmp/package"),
            Path("/tmp/public.hex"),
            verifier_script=Path("/repo/verify-signed-package.py"),
            runner=runner,
        )
        self.assertEqual(
            calls[0][0],
            [
                sys.executable,
                "/repo/verify-signed-package.py",
                "/tmp/package",
                "--public-key",
                "/tmp/public.hex",
            ],
        )
        self.assertEqual(calls[0][1]["check"], False)


class SerialPortTest(unittest.TestCase):
    def test_rejects_unstable_tty_name(self) -> None:
        with self.assertRaisesRegex(ngu.DevicePathError, "by-id"):
            ngu.validate_stable_serial_path(Path("/dev/ttyACM0"))

    def test_open_configures_raw_port_without_hupcl_or_modem_ioctl(self) -> None:
        controls = [0] * 32
        attributes = [0xFFFF, 0xFFFF, termios_flags(), 0xFFFF, 0, 0, controls]
        configured: list[list[object]] = []
        with mock.patch.object(
            ngu, "validate_stable_serial_path", return_value=Path("/dev/serial/by-id/watch")
        ), mock.patch.object(ngu, "competing_device_holders", return_value=[]), mock.patch.object(
            ngu.os, "open", return_value=73
        ) as opened, mock.patch.object(
            ngu.os, "close"
        ) as closed, mock.patch.object(ngu.fcntl, "ioctl") as ioctl, mock.patch.object(
            ngu.termios, "tcgetattr", return_value=attributes
        ), mock.patch.object(
            ngu.termios,
            "tcsetattr",
            side_effect=lambda _fd, _when, value: configured.append(value),
        ):
            port = ngu.PosixSerialPort(Path("/dev/serial/by-id/watch"))
            port.close()
        flags = opened.call_args.args[1]
        self.assertTrue(flags & ngu.os.O_NOCTTY)
        self.assertTrue(flags & ngu.os.O_NONBLOCK)
        cflag = configured[0][2]
        self.assertFalse(cflag & ngu.termios.HUPCL)
        self.assertFalse(cflag & ngu.termios.PARENB)
        self.assertFalse(cflag & ngu.termios.CSTOPB)
        self.assertTrue(cflag & ngu.termios.CLOCAL)
        self.assertTrue(cflag & ngu.termios.CREAD)
        ioctl.assert_called_once_with(73, ngu.termios.TIOCEXCL)
        closed.assert_called_once_with(73)

    def test_open_refuses_competing_serial_monitor(self) -> None:
        with mock.patch.object(
            ngu, "validate_stable_serial_path", return_value=Path("/dev/serial/by-id/watch")
        ), mock.patch.object(
            ngu, "competing_device_holders", return_value=[912]
        ), mock.patch.object(ngu.os, "open") as opened:
            with self.assertRaisesRegex(ngu.DevicePathError, "close serial monitors"):
                ngu.PosixSerialPort(Path("/dev/serial/by-id/watch"))
        opened.assert_not_called()


def termios_flags() -> int:
    value = ngu.termios.CS7 | ngu.termios.PARENB | ngu.termios.CSTOPB | ngu.termios.HUPCL
    if hasattr(ngu.termios, "CRTSCTS"):
        value |= ngu.termios.CRTSCTS
    return value


class MemoryPort:
    def __init__(self, reads: list[bytes]) -> None:
        self.reads = deque(reads)
        self.writes: list[bytes] = []

    def write_all(self, data: bytes, _timeout: float) -> None:
        self.writes.append(data)

    def read(self, _timeout: float) -> bytes:
        if not self.reads:
            raise ngu.AckTimeout("empty")
        return self.reads.popleft()

    def reconnect(self, _timeout: float) -> None:
        pass

    def close(self) -> None:
        pass


class ReenumeratingHealthPort(MemoryPort):
    def __init__(self, reads: list[bytes | Exception]) -> None:
        super().__init__([])
        self.health_reads = deque(reads)
        self.reconnects = 0

    def read(self, _timeout: float) -> bytes:
        if not self.health_reads:
            raise ngu.AckTimeout("empty")
        value = self.health_reads.popleft()
        if isinstance(value, Exception):
            raise value
        return value

    def reconnect(self, _timeout: float) -> None:
        self.reconnects += 1


class FramedLinkTest(unittest.TestCase):
    def test_coalesced_status_frames_are_not_discarded(self) -> None:
        first = status(9, ngu.UPDATE_STATE_RECEIVING, expected=20, received=10)
        second = status(9, ngu.UPDATE_STATE_RECEIVING, expected=20, received=20)
        port = MemoryPort(
            [ngu.encode_envelope(status_bytes(first)) + ngu.encode_envelope(status_bytes(second))]
        )
        link = ngu.FramedUsbLink(port)  # type: ignore[arg-type]
        self.assertEqual(link.receive_status(9, 1), first)
        self.assertEqual(link.receive_status(9, 1), second)

    def test_health_requires_pending_boot_before_acceptance(self) -> None:
        port = MemoryPort([])
        link = ngu.FramedUsbLink(port)  # type: ignore[arg-type]
        link._record_log(b"OTA_HEALTH_ACCEPTED state=VALID pending=0\n")
        self.assertIsNone(link._health_evidence("9.8.7", 12))
        link._record_log(
            b"USB_UPDATE_BOOT version=9.8.7 secure=12 pending=1 "
            b"state=PENDING_VERIFY\n"
            b"OTA_HEALTH_ACCEPTED state=VALID pending=0\n"
        )
        self.assertIsNotNone(link._health_evidence("9.8.7", 12))
        self.assertIsNone(link._health_evidence("9.8.8", 12))

    def test_exchange_ignores_stale_data_ack_before_query_ack(self) -> None:
        stale = status(
            9,
            ngu.UPDATE_STATE_RECEIVING,
            expected=20,
            received=20,
            acknowledged_opcode=ngu.UPDATE_DATA,
        )
        query = status(
            9,
            ngu.UPDATE_STATE_RECEIVING,
            expected=20,
            received=20,
            acknowledged_opcode=ngu.UPDATE_STATUS_QUERY,
        )
        port = MemoryPort(
            [ngu.encode_envelope(status_bytes(stale)) + ngu.encode_envelope(status_bytes(query))]
        )
        link = ngu.FramedUsbLink(port)  # type: ignore[arg-type]
        observed = link.exchange(ngu.build_control_payload(ngu.UPDATE_STATUS_QUERY, 9), 9, 1)
        self.assertEqual(observed.acknowledged_opcode, ngu.UPDATE_STATUS_QUERY)

    def test_reenumerated_health_probe_requires_exact_image_marker(self) -> None:
        query_status = status(
            9,
            ngu.UPDATE_STATE_IDLE,
            signature=0,
            acknowledged_opcode=ngu.UPDATE_STATUS_QUERY,
        )
        port = ReenumeratingHealthPort(
            [
                ngu.LinkDisconnected("re-enumerating"),
                b"USB_UPDATE_BOOT version=9.8.7 secure=12 pending=1 "
                b"state=PENDING_VERIFY\n"
                + ngu.encode_envelope(status_bytes(query_status)),
                ngu._HEALTH_ACCEPTED + b"\n",
            ]
        )
        link = ngu.FramedUsbLink(port)  # type: ignore[arg-type]
        evidence = link.wait_for_reboot_health(9, "9.8.7", 12, 0.5)
        self.assertEqual(port.reconnects, 1)
        self.assertIn(b"version=9.8.7", evidence.pending_boot)
        self.assertGreaterEqual(len(port.writes), 2)


class WindowedWatch:
    def __init__(self, package: ngu.SignedPackage, resume: int = 0) -> None:
        self.package = package
        self.resume = resume
        self.received = resume
        self.session = ngu.derive_session(package)
        self.trace: list[tuple[str, int]] = []
        self.pending: deque[ngu.UpdateStatus] = deque()
        self.health_waited = False

    def send(self, payload: bytes, _timeout: float) -> None:
        self.assert_data(payload)
        offset = struct.unpack_from("<I", payload, 10)[0]
        data = payload[14:]
        self.trace.append(("send", offset))
        self.received = offset + len(data)
        self.pending.append(
            status(
                self.session,
                ngu.UPDATE_STATE_RECEIVING,
                received=self.received,
                expected=self.package.image_size,
            )
        )

    def assert_data(self, payload: bytes) -> None:
        if payload[1] != ngu.UPDATE_DATA:
            raise AssertionError("window send was not update data")

    def receive_status(self, expected_session: int, _timeout: float) -> ngu.UpdateStatus:
        self.trace.append(("receive", self.received))
        if expected_session != self.session or not self.pending:
            raise AssertionError("unexpected status receive")
        return self.pending.popleft()

    def exchange(
        self,
        payload: bytes,
        expected_session: int,
        _timeout: float,
        *,
        accept_foreign_session: bool = False,
    ) -> ngu.UpdateStatus:
        del accept_foreign_session
        if expected_session != self.session:
            raise AssertionError("unexpected session")
        opcode = payload[1]
        self.trace.append(("exchange", opcode))
        if opcode == ngu.UPDATE_STATUS_QUERY:
            state = ngu.UPDATE_STATE_RECEIVING if self.received else ngu.UPDATE_STATE_IDLE
            signature = ngu.SIGNATURE_VERIFIED if self.received else 0
            expected = self.package.image_size if self.received else 0
            return status(
                self.session,
                state,
                received=self.received,
                expected=expected,
                signature=signature,
            )
        if opcode == ngu.UPDATE_BEGIN:
            return status(
                self.session,
                ngu.UPDATE_STATE_RECEIVING,
                received=self.received,
                expected=self.package.image_size,
            )
        if opcode == ngu.UPDATE_FINISH:
            return status(
                self.session,
                ngu.UPDATE_STATE_READY_TO_REBOOT,
                received=self.received,
                expected=self.package.image_size,
            )
        raise AssertionError(f"unexpected exchange opcode {opcode}")

    def reconnect(self, _timeout: float) -> None:
        self.trace.append(("reconnect", 0))

    def clear_log_history(self) -> None:
        self.trace.append(("clear-log", 0))

    def wait_for_reboot_health(
        self, session: int, app_version: str, secure_version: int, _timeout: float
    ) -> ngu.HealthEvidence:
        if session != self.session or app_version != self.package.app_version or secure_version != self.package.secure_version:
            raise AssertionError("wrong expected reboot identity")
        self.health_waited = True
        return ngu.HealthEvidence(b"USB_UPDATE_BOOT pending=1", ngu._HEALTH_ACCEPTED)


class TimeoutWindowedWatch(WindowedWatch):
    def __init__(self, package: ngu.SignedPackage) -> None:
        super().__init__(package)
        self.first_window = True
        self.first_boundary = 0

    def send(self, payload: bytes, _timeout: float) -> None:
        self.assert_data(payload)
        offset = struct.unpack_from("<I", payload, 10)[0]
        data = payload[14:]
        self.trace.append(("send", offset))
        if self.first_boundary == 0:
            self.first_boundary = offset + len(data)
        if not self.first_window:
            self.received = offset + len(data)
            self.pending.append(
                status(
                    self.session,
                    ngu.UPDATE_STATE_RECEIVING,
                    received=self.received,
                    expected=self.package.image_size,
                )
            )

    def receive_status(self, expected_session: int, timeout: float) -> ngu.UpdateStatus:
        if self.first_window:
            self.first_window = False
            self.received = self.first_boundary
            self.pending.clear()
            self.trace.append(("timeout", self.received))
            raise ngu.AckTimeout("ambiguous delivery")
        return super().receive_status(expected_session, timeout)


class InstallerTest(unittest.TestCase):
    def test_wire_status_codes_match_firmware_status_enum_offset(self) -> None:
        failure = status(9, ngu.UPDATE_STATE_FAILED, result=4)
        with self.assertRaisesRegex(ngu.ProtocolError, "invalid state"):
            ngu.UsbUpdateInstaller._require_success(failure, "test")

    def test_resumes_and_uses_three_frame_window(self) -> None:
        firmware = bytes((index % 251 for index in range(1400)))
        with tempfile.TemporaryDirectory() as temporary:
            package = make_package(Path(temporary), firmware)
            watch = WindowedWatch(package, resume=230)
            evidence = ngu.UsbUpdateInstaller(
                watch, package, ack_timeout=0.1, reconnect_timeout=0.1, health_timeout=0.1
            ).install()
        first_send = watch.trace.index(("send", 230))
        self.assertEqual(
            watch.trace[first_send : first_send + 4],
            [("send", 230), ("send", 460), ("send", 690), ("receive", 920)],
        )
        self.assertEqual(watch.received, len(firmware))
        self.assertTrue(watch.health_waited)
        self.assertIn(b"pending=1", evidence.pending_boot)

    def test_timeout_queries_offset_before_replaying_unaccepted_frames(self) -> None:
        firmware = bytes((index % 239 for index in range(1000)))
        with tempfile.TemporaryDirectory() as temporary:
            package = make_package(Path(temporary), firmware)
            watch = TimeoutWindowedWatch(package)
            ngu.UsbUpdateInstaller(
                watch, package, ack_timeout=0.1, reconnect_timeout=0.1, health_timeout=0.1
            ).install()
        sends = [offset for action, offset in watch.trace if action == "send"]
        self.assertEqual(sends.count(0), 1)
        self.assertGreaterEqual(sends.count(230), 2)
        timeout_index = watch.trace.index(("timeout", 230))
        self.assertEqual(watch.trace[timeout_index + 1], ("exchange", ngu.UPDATE_STATUS_QUERY))


if __name__ == "__main__":
    unittest.main()
