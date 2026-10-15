# Warehouse Safety-Patrol Balancing Robot — Course Project (Group 1)

> **English summary.** The full documentation is in Chinese ([`README.md`](README.md),
> [`docs/`](docs/)); this file exists so the repository is usable by anyone who does not read Chinese.

## What this is

A course project for *Measurement & Control System Design*: a self-balancing two-wheeled robot
that patrols a warehouse floor, samples **gas (MQ-2 / MQ-135)** and **temperature/humidity (DHT11)**,
judges whether readings cross thresholds, raises a local alarm, and reports state to a PC
monitoring application. A 4-person team built it end to end:

| Role | Scope | Where |
|---|---|---|
| **A1** | Circuit: new baseboard (schematic + PCB + fabrication) | `hardware/`, `docs/design_hardware/` |
| **A2** | Mechanics: structure, assembly drawing, renderings | `hardware/`, `docs/design_mechanics/` |
| **B** | **Firmware (STM32F405RGT6)** | [`firmware/`](firmware/) |
| **C** | PC application (Qt) + protocol | [`host/`](host/) |

## Firmware in one page

* **Platform**: MiniBalance V5.0 board, STM32F405RGT6 @168 MHz, ST **StdPeriph** library (not HAL).
* **Architecture — hybrid**: the *balance control loop stays in the MPU6050 interrupt* (unchanged
  vendor timing, 5 ms/10 ms ticks), while acquisition, communication, the patrol state machine and
  the UI run as **four FreeRTOS tasks** (Kernel V11.1.0 vendored, heap_4).
  The two interrupts are higher priority than `configMAX_SYSCALL_INTERRUPT_PRIORITY`, therefore they
  **never call any FreeRTOS API** — verified statically by `tools/fw_prep/check_isr_safety.py`.
* **Custom protocol**: `0xAA55` + command + length + **CRC-16/MODBUS**; streaming decoder with a
  100 ms frame timeout; SPSC ring buffer between ISR and task; TX mutex shared with `printf`.
  Two peer links: **Bluetooth (USART2)** and a **wired link (USART1, the on-board CH340G/MicroUSB)**.
* **Sensing**: MQ-2 (PA5) and MQ-135 (PA6) on **ADC2** (ADC1 stays with the interrupt for battery
  sensing — a task must not share a peripheral with a higher-priority ISR); DHT11 on PA7 with retries;
  `Rs/R0` as the primary quantity, temperature/humidity compensation (off by default), multi-frame
  confirmation with hysteresis, adaptive 1 Hz ↔ 5 Hz sampling.
* **Safety**: 500 ms link-loss detection → **latched** safe stop (never auto-restarts), emergency-stop
  command, low-battery and tilt protection, over-threshold buzzer.

## Build the firmware (no Keil needed)

```bash
cd firmware
make                 # produces build/MiniBalance.elf/.hex/.bin/.map
make USE_RTOS=0      # bare-metal reference build (vendor super-loop)
```

Requires `arm-none-eabi-gcc` on `PATH` (any location). If it is elsewhere:

```bash
make PREFIX=/path/to/GNU-tools-for-STM32/bin/arm-none-eabi-
# or set FW_ARM_BIN / STM32CUBECLT; the check scripts auto-detect (tools/fw_prep/toolchain.py)
```

Flashing: the control board's MicroUSB (CH340G) supports serial ISP — use FlyMcu with
`firmware/build/MiniBalance.hex` (DTR/RTS auto-ISP), or SWD via ST-Link (`make flash`).

## Offline verification (no hardware required)

```bash
python tools/fw_prep/run_all_tests.py     # all nine checks, summarized
# on a GBK console, set PYTHONIOENCODING=utf-8 first (the summary table contains non-ASCII)
```

| Check | What it proves |
|---|---|
| 4 native unit-test suites (protocol 117 / sensor 1235 / **patrol 80** / param 29 asserts) | algorithms and the decoder are correct on a PC |
| Shared protocol vectors ([`docs/protocol/test_vectors.json`](docs/protocol/test_vectors.json)) | Python reference ↔ host C++ ↔ firmware all agree (CRC 4 + frames 16 + streams 6) |
| `check_vector_table.py` / `check_isr_safety.py` / `pin_check.py` / `tune.py --selftest` | interrupt vectors, "no RTOS API inside ISRs", pin map vs. the finished PCB, tuner codec — **9 checks in total** |
| `make strict` (conversion/shadow/missing-prototype warnings) | 19 self-written / hand-modified files compile with **0 warnings** — ⚠️ **run `make clean` first**, otherwise an incremental build may compile nothing and "0 warnings" is a false green |

Current footprint: **Flash ≈ 66.5 KB / RAM ≈ 32.8 KB** (measured 2026-10-15; only the rounded
figures are quoted on purpose, so they do not go stale). ⚠️ The bare-metal comparison build
(`make USE_RTOS=0`) **does not compile at the moment** (see `firmware/README.md` §六).

Protocol version: **v1.2** (v1.1 frozen + the two new uplink frames **`0x15` diagnostics** (N=16, 1 Hz)
and **`0x16` control-loop internals** (N=20, 1 Hz; byte 19 = firmware version `(month<<4)|day`, now `0xAF`)).

Bench state (2026-10-15): only the **MQ-135 is fitted, in `J13`** (`J10` empty, the MQ-2 module is
burnt and awaiting replacement); DHT11 fine; battery 11.8 V; **OLED displays normally** —
earlier "no sensors ready" / "OLED garbled" notes are **resolved**. Bluetooth: Windows SPP pairs but
**exposes no COM port**, and this Qt build has no Qt Bluetooth module — so wiring (USART1) is the
primary link.

## PC-side tuning tool

```bash
python tools/fw_prep/tune.py --selftest              # offline, no board
python tools/fw_prep/tune.py -p COM5 --dump          # read all PID gains
python tools/fw_prep/tune.py -p COM5 --set 3=85      # change one gain (takes effect immediately)
python tools/fw_prep/tune.py -p COM5 --observe 20 --csv run1.csv   # 100 Hz scope + statistics
```

## Documentation map

| Doc | Content |
|---|---|
| [`docs/design_firmware/程序说明文档.md`](docs/design_firmware/程序说明文档.md) | Program design document (flow charts, task split, protocol implementation, interfaces) |
| [`docs/protocol/communication_protocol.md`](docs/protocol/communication_protocol.md) | Wire protocol definition |
| [`docs/design_firmware/B阶段开工准备.md`](docs/design_firmware/B阶段开工准备.md) | Firmware work log: environment, milestones P0–P5, pitfalls, review records |
| [`docs/handoff/交接说明.md`](docs/handoff/交接说明.md) | Project handover: verified facts, decisions, next steps |
| [`docs/决策记录.md`](docs/决策记录.md) | Decision log (numbered, with rationale) |
| [`AGENTS.md`](AGENTS.md) | Instructions for AI coding agents (paths, hard-won rules, capability limits) |

## Notes

* The original vendor MiniBalance source and the school's material are **not** redistributed here
  (`Base/` is git-ignored); the firmware tree contains only what is needed to build.
* Documents and code comments are in Chinese; identifiers and directory names are English.
* Personal data (student IDs, names) has been deliberately kept out of this public repository.
