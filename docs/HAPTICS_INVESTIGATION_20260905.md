# Haptics investigation — 2026-09-05

## Conclusion

Nightglass 0.2.11 deliberately disables haptics. There is also no application
integration that requests vibration. The earlier conclusion that the actuator
is physically absent is not established by the retained evidence: software
commands and PMIC configuration readback are not motor-presence detection.

Investigation only: no firmware/source behavior changed, no USB session opened,
no motor energized, no tests or builds run, and no installation performed.

## Confirmed firmware findings

| Finding | Evidence |
| --- | --- |
| Haptics disabled at compile time | `components/nightglass_services/src/hardware.cpp:31` sets `kHapticActuatorPresent=false`. |
| Motor supply intentionally shut off | `hardware.cpp:614` selects `disable_haptic_supply`; `ready` remains false. |
| Pulse requests rejected | `hardware.cpp:789` rejects a request when the haptic service is not ready. |
| No application pulse requests | Source search found only the `request_haptic` declaration and definition, no callers. Notifications request audio at `connectivity.cpp:471`; alarms/timers request audio at `clock.cpp:525`. |
| Diagnostic message overstates knowledge | `components/nightglass_ui/src/shell.cpp:4808` displays “No fitted actuator detected” from the fixed boolean, not a hardware detection procedure. |

The hardware service, public hardware header, and UI source exactly match the
accepted 0.2.11 build checkout, `~/projects/watch/nightglass-bus-quiet-20260905`.
SHA256 values, respectively:

- Hardware service: `baaa21433d9a5ee98cfffe52b4540373c062d82deb66b49bd11c22c6ca22d5a9`
- Header: `482611e6e0472817a68bbf43f5ca21badc4f3e57bcf133cee03a2d4e6e2b9304`
- UI: `02987a67365384431c85575c11760d2198fdd46e42c5157f5ee8005ac2c166e1`

The disabled flag and diagnostic wording also exist in the earlier
`nightglass-wake-fix-20260905` source. This is not newly introduced by the
accepted wake repair.

## Hardware and historical evidence

The retained official Waveshare v1.0 schematic shows a motor connection path:
ALDO3 through R7 (0 ohms) to P1; P2 through Q1 (MMBT3904) to ground. GPIO18
controls Q1 through R12 (4.7 kohms), with R13 (47 kohms) pulling its control low.
This establishes schematic support for an attached motor, not whether one is
fitted to this particular watch.

Git commit `107598376e2a2f2921266928c7a956ae10b1724b`, dated August 30,
introduced the disabled flag and the “not fitted” wording after reported lack
of mechanical response. The earlier diagnostic code verifies ALDO3 configuration
registers and checks `gpio_set_level` return values. These checks do not measure
voltage at P1/P2, motor current, transistor switching, or physical vibration.
No retained motor-presence inspection or analog measurement was located in
the inspected project artifacts. A missing, disconnected, faulty, or otherwise
nonresponding motor cannot be distinguished from these records alone.

Schematic crop and prior wake evidence:
`~/projects/watch/backups/nightglass-wake-20260905-pbdibkns/vendor/schematic-motor.png`.
Independent read-only hardware/provenance review corroborated this distinction.

## Smallest supported next steps

1. Verify the exact unit's fitted motor and its P1/P2 connections before another
   firmware experiment. Presence must not be inferred from the current diagnostic.
2. If a motor is present, use a bounded diagnostic pulse with actual electrical
   and mechanical observation to separate drive-circuit trouble from motor trouble.
   If none is fitted, suitable hardware is required before software can provide
   haptic feedback.
3. Only after that result, enable the verified capability and connect intentional
   notification/UI events. Changing the compile-time flag alone will not produce
   everyday feedback because there are currently no callers.
4. Replace the misleading detection claim in a future firmware change with a
   truthful state such as “Haptics disabled — motor not verified.” Keep the
   installed, accepted wake fix untouched during this investigation.
