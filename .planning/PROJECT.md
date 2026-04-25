# meridian (cortex-m-emu)

## What This Is

Full-system ARM Cortex-M emulator in pure C11. Runs unmodified FreeRTOS-Kernel,
Zephyr-lite, STM32F103 firmware. Native x86-64 JIT, GDB stub, MPU, VFPv4-SP.
Foundation for a three-product platform: CI runner, learning IDE, security probe.

## Core Value

f(state, time, events) -> state' is deterministic, fast, snapshotable, reversible.
All three product faces are different UIs over the same deterministic kernel.

## Current Milestone: v2.0 Time-Travel + Product Platform

**Goal:** turn the engine into a usable product — add the time-travel kernel
(unique USP vs QEMU/Renode) and ship the three product faces (CI, learn, probe).

**Target features:**
- Snapshot + replay + rewind kernel (deterministic, COW)
- Direct block chaining + native LDR/STR (target 100M+ IPS)
- WASM port + web IDE (educational platform)
- Python API + pytest plugin + Docker (CI runner)
- Landing page

## Requirements

### Validated (v1.0)

- ARMv6-M + ARMv7-M ISA + IT block + bitfield + table-branch
- VFPv4-SP (VLDR/VSTR/VADD/VSUB/VMUL/VDIV/VSQRT/VFMA/VPUSH/VPOP)
- NVIC (240 IRQ lines, priority pick), SysTick, MSR/MRS, PendSV
- MPU 8 regions, fault escalation
- GDB stub (TCP RSP), STM32 mocks (RCC/GPIO/USART), DWT
- x86-64 JIT codegen (mmap RWX, ~30M IPS hybrid)
- 62 tests green (48 unit + 14 firmware)

### Active (v2.0)

See REQUIREMENTS.md.

### Out of Scope (v2.0)

- ARMv8-M TrustZone — niche, defer
- Cortex-A (Linux loads) — different target / product
- VFP double-prec — single covers 90% of M4F code
- Full symbolic execution — v3.0 probe path
- Multi-core (SMP) — Cortex-M is uniprocessor by spec

## Context

Brownfield: 12 phases shipped. Real x86 JIT in p12 lifts us from school
project to potentially competitive. Time-travel is the unique USP — neither
QEMU nor Renode does proper rewind for ARM Cortex-M.

Three product paths share one kernel:
- meridian-ci (B2B): CI runner replaces hardware farms
- meridian-learn (B2C): web IDE for ARM/RTOS courses
- meridian-probe (B2B-niche): security research with replay

## Constraints

- **Tech stack**: C11 only in core, no external deps — portability + audit
- **Style**: olympiad — 1-2 letter locals, no .unwrap, multi-file, ASCII math
- **Determinism**: no rand/clock inside cpu, all I/O via event log
- **Commits**: short ASCII-only math, no claude refs
- **Performance target v2.0**: 100M+ IPS native after direct chaining

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Real x86 codegen vs interp-cache | speed path 30M -> 100M+ IPS | ✓ p12 |
| Three products on one kernel | f(state,t,e) universal | — Pending |
| WASM for educational reach | no-install web demo | — Pending |
| Determinism over raw speed | replay/rewind needs exactness | — Pending |
| MIT license | open core, paid services | ✓ |

---
*Last updated: 2026-04-26 after p12 (real x86 jit), starting v2.0*
