# OTA production campaign

Updated: 2026-08-31

This ledger is the authoritative continuation state for making Nightglass OTA
updates production-usable. A phase is complete only when its acceptance gate
passes; a commit, build, or partial implementation is not completion.

| ID | Phase | State | Acceptance gate |
|---|---|---|---|
| OTA-P1 | Baseline and verifier | completed | Clean checkpoint `d428f19`; deterministic release gate passes; P-256 known-answer, mutation, malformed-DER, wrong-key, high/low-S, and Python/C++ payload-parity tests pass with unsigned development disabled. |
| OTA-P2 | Signing-key custody | in progress | Production P-256 private key exists only in an approved host-owned protected facility; the public SEC1 point is reproducibly exported and provisioned into firmware; no private material exists in Git, build output, logs, shell arguments, APK, or device firmware. |
| OTA-P3 | Authenticated transport | in progress | Updated Android companion sends an explicitly confirmed signed package over the encrypted, pinned BLE link; fixed-size callbacks enqueue only bounded work; the update worker supports status, abort, exact sequential chunks, and reconnect-safe failure handling through `UpdateService`. |
| OTA-P4 | Security and release review | pending | Independent security and QA reviews have no unresolved ship blockers; firmware and APK tests pass; deterministic release artifacts match; release config contains the expected public key and no unsigned bypass. |
| OTA-P5 | Rollback HIL | pending | Wrong-key, corrupt-image, interrupted-transfer, pending-verify, forced-health-failure rollback, healthy acceptance, BLE reconnect, USB inactivity, display redraw, and audio coexistence checks pass on the attached watch. |
| OTA-P6 | Companion and watch deployment | pending | Reviewed companion APK is installed, watch is updated through the signed transport rather than a raw slot write, both recover automatically, and final hashes plus boot-state evidence are recorded. |

## Active lanes

- **transport implementation** — current action: firmware and Android BLE
  transport, queue ownership, explicit confirmation, progress, and tests;
  next gate: both targets compile and transport unit tests pass; artifact:
  local commit.
- **key custody and threat review** — current action: select the host-native
  protected store and key lifecycle; next gate: private key is non-exporting in
  normal operation and public-key provisioning is reproducible; artifact:
  custody runbook plus provisioned public point.
- **rollback QA** — current action: convert the documented HIL outline into an
  executable evidence matrix; next gate: every destructive step has a
  precondition, expected boot state, and recovery path; artifact: HIL report.
- **integration and release** — current action: maintain the clean tree,
  reconcile lane commits, run cross-review and deterministic builds; next gate:
  OTA-P4; artifact: firmware/APK hashes and review disposition.

## Repair queue

Empty at campaign start. Add each review or test failure here with its owning
lane and exact gate to rerun.

## External waits

- Android companion installation waits for an authorized phone to appear in
  `adb devices`.
- Irreversible Secure Boot, flash-encryption, and eFuse anti-rollback
  provisioning are outside this campaign and require a separately reviewed
  recovery plan.

