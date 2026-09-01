#!/usr/bin/env python3
"""Parse bounded Nightglass OTA serial evidence and enforce requested milestones."""

from __future__ import annotations

import argparse
import json
import pathlib
import re


BOOT = re.compile(
    r"OTA_BOOT running=(?P<running>[^@ ]+)@(?P<running_address>0x[0-9a-fA-F]+) "
    r"configured=(?P<configured>[^@ ]+)@(?P<configured_address>0x[0-9a-fA-F]+) "
    r"state=(?P<state>\d+) pending=(?P<pending>[01]) "
    r"rollback_possible=(?P<rollback>[01])"
)
READY = re.compile(
    r"OTA_READY target=(?P<target>[^@ ]+)@(?P<address>0x[0-9a-fA-F]+) "
    r"state=(?P<state>-?\d+) readback_sha256=verified bytes=(?P<bytes>\d+)"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--require-ready", action="store_true")
    parser.add_argument("--require-pending", action="store_true")
    parser.add_argument("--require-accepted", action="store_true")
    parser.add_argument("--require-rollback", action="store_true")
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    boot_matches = list(BOOT.finditer(text))
    boots = [match.groupdict() for match in boot_matches]
    ready = [match.groupdict() for match in READY.finditer(text)]
    accepted_marker = text.find("OTA_HEALTH_ACCEPTED state=VALID pending=0")
    rollback_marker = text.lower().find("requesting rollback")
    accepted = accepted_marker >= 0
    rollback = rollback_marker >= 0
    if args.require_ready and not ready:
        raise SystemExit("missing OTA_READY readback evidence")
    if args.require_pending and not any(item["pending"] == "1" for item in boots):
        raise SystemExit("missing pending-verification OTA_BOOT evidence")
    if args.require_accepted:
        pending_before = [match for match in boot_matches
                          if match.start() < accepted_marker and match["pending"] == "1"]
        stable_after = [match for match in boot_matches
                        if match.start() > accepted_marker and match["pending"] == "0"]
        if not accepted or not pending_before or not stable_after or \
                stable_after[-1]["running"] != pending_before[-1]["running"]:
            raise SystemExit("missing accepted-image reboot evidence")
    if args.require_rollback:
        pending_before = [match for match in boot_matches
                          if match.start() < rollback_marker and match["pending"] == "1"]
        restored_after = [match for match in boot_matches
                          if match.start() > rollback_marker and match["pending"] == "0"]
        if not rollback or not pending_before or not restored_after or \
                restored_after[-1]["running"] == pending_before[-1]["running"]:
            raise SystemExit("missing proven rollback slot transition")
    print(json.dumps({
        "boots": boots,
        "ready": ready,
        "accepted": accepted,
        "rollback_requested": rollback,
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
