# M5 PaperColor C151 post-flash serial bench

This harness evaluates bounded, machine-readable evidence after flashing. It does **not** flash, auto-select a port, modify the device, contact MyAI, or claim a physical hardware pass.

## Run

Install the only live-hardware dependency if needed:

```sh
python3 -m pip install pyserial
```

Find the serial name yourself, then pass that exact value. Wildcards and values such as `auto` or `first` are rejected.

```sh
python3 scripts/papercolor_c151_serial_bench.py \
  --port /dev/cu.usbmodem1234 \
  --baud 115200 \
  --timeout 90 \
  --report artifacts/c151-bench.json
```

The default command is only `status` (`--diagnostic diag` is equivalent). Hardware-output commands are never sent unless explicitly enabled:

```sh
python3 scripts/papercolor_c151_serial_bench.py \
  --port /dev/cu.usbmodem1234 \
  --test-audio --test-rgb --test-display \
  --observe speaker=pass --observe led_left=pass --observe screen=pass \
  --report artifacts/c151-output-tests.json
```

Allowed writes are exactly `status`, `diag`, `sound-test`, `led-test`, and `screen-test`. The harness never sends `reboot`, recovery, mapping, deletion, formatting, or other mutation commands.

## Acceptance boundary

Machine-verifiable checks require the exact non-empty startup chain `RESET_REASON:0..15` → `BOOT:<firmware>` → `BOARD:28` → `PM1:READY` → `HARDWARE_READY:READY`. Values follow the pinned ESP-IDF 6 enum exactly: `0 UNKNOWN`, `1 POWERON`, `2 EXT`, `3 SW`, `4 PANIC`, `5 INT_WDT`, `6 TASK_WDT`, `7 WDT`, `8 DEEPSLEEP`, `9 BROWNOUT`, `10 SDIO`, `11 USB`, `12 JTAG`, `13 EFUSE`, `14 POWER_GLITCH`, and `15 CPU_LOCKUP`; values `16` and above are rejected until the pinned SDK deliberately adds them. Initial hard-reset value `0` is preserved as typed `UNKNOWN` evidence and emits a report warning—it is never relabeled as `POWERON`. A matching device command echo and status/diagnostic object must arrive after the successfully written command, in the same fresh receive epoch. The response may be either the legacy bounded JSON identity object or the strict native ESP-IDF status tuple; boot and hardware identity are still independently verified from the startup chain. Wi-Fi AP/connected state, a single shared six-digit MyAI/Inkloop code when emitted, blocked activation/payment states, runtime errors, optional test acknowledgements, and credential hygiene are also checked. If firmware emits `REBOOTING`, the later reset must be the controlled software-reset value `3`, followed by a complete startup chain.

When the port opens, queued input is consumed as pre-command evidence. Immediately before every command the remaining receive backlog is boundedly drained/reset, partial stale input is discarded, and a new RX epoch is recorded with the successful command-write sequence/time. Echoes or responses from an earlier epoch cannot acknowledge the new command. Adapters must provide pyserial-compatible `in_waiting` or `reset_input_buffer`; otherwise the tool stops safely.

Malformed JSON/events, invalid UTF-8, lines over 4096 bytes, a partial final line, excessive JSON depth/cardinality, `NaN`/infinity, two different codes, suspicious secret labels, or token/secret output fail closed. Dynamic secret-bearing keys and event names; `key:value`, `key=value`, and `key value`; split labels such as `api key`/`access token`; and Authorization Basic/Bearer/Token/Digest schemes are removed from console and strict JSON (`allow_nan=false`) output. The raw transcript is not saved.

The report separately records button, voice, image, sleep, and wake events. Screen quality, audible speaker output, microphone quality, physical LED side/color/animation, button feel, full-refresh artifacts, deep-sleep current, and wake behavior remain human observations. Even with `--observe ...=pass`, the report says `HUMAN_OBSERVATIONS_RECORDED_NOT_VERIFIED`, never “physical PASS.” `verdict.machine` is derived only from machine checks; an operator failure appears independently in `verdict.human` and makes `verdict.overall` fail without relabeling it as a machine failure.

Run parser tests without pyserial or hardware:

```sh
python3 -m unittest -v tests/test_papercolor_c151_serial_bench.py
```

Exit code `0` means required serial checks passed and no human failure was recorded, while physical verification remains incomplete; `1` means a machine check or operator observation failed; `2` means the harness could not run safely (bad arguments, missing pyserial, port/report/serialization error).
