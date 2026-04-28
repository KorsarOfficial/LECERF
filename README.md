# LECERF

Full-system ARM Cortex-M emulator in pure C11 with **time-travel** (snapshot
+ rewind + replay) and a real x86-64 JIT (`mmap` RWX, ~30M IPS hybrid).

Runs Cortex-M0/M0+/M1/M3/M4/M4F/M7 firmware (Thumb-1 + Thumb-2 + VFPv4-SP)
without hardware. Boots **FreeRTOS-Kernel V10.6.2** and **STM32F103** firmware
unmodified, exposes a GDB Remote Serial Protocol stub for `arm-none-eabi-gdb`.

```
firmware.bin
    |
    v
+------------------------------------------------------------+
| LECERF core                                              |
|                                                            |
|  fetch -> dcache -> decode -> exec   (Thumb-1 / Thumb-2)   |
|                       |                                    |
|                       +--> jit hot-block detector          |
|                              +--> codegen.c (x86-64 RWX)   |
|                                    rdi=cpu, rsi=bus -> b8  |
|                                                            |
|  NVIC-240 + SCB + SysTick + DWT + MPU-8r + VFPv4-SP        |
|  bus: flat regions + MMIO; periph: UART, STM32, ETH        |
|                                                            |
|  tt: f(state, time, events) -> state'                      |
|       ev_log_t (UART RX, IRQ, ETH RX) + snap stride K      |
|       tt_create / tt_on_cycle / tt_rewind / tt_step_back   |
|       tt_replay / tt_diff (record-and-replay determinism)  |
+------------------------------------------------------------+
    |                                |
    v                                v
  stdout (UART tx)            arm-none-eabi-gdb
                              (target remote :1234)
```

## What makes it different

| Feature                          | LECERF  | QEMU-system-arm | Renode |
|----------------------------------|-----------|-----------------|--------|
| Cortex-M ISA + VFPv4-SP          | yes       | yes             | yes    |
| Boots FreeRTOS unmodified        | yes       | yes             | yes    |
| Real x86-64 JIT (RWX mmap)       | yes ~30M  | TCG ~100M       | no     |
| **Snapshot + restore byte-eq**   | yes 0.14ms| no              | yes    |
| **Rewind <100ms across 1M cyc**  | yes 0.3ms | no              | no     |
| **step_back(N) reverse-step**    | yes       | no              | no     |
| **Record + replay deterministic**| yes       | no              | partial|
| GDB RSP stub                     | yes       | yes             | yes    |
| Single C11 binary, no deps       | yes       | no              | no     |

`f(state, time, events) -> state'` is empirically deterministic, snapshotable,
and reversible — that is the unique cut over QEMU/Renode for ARM Cortex-M.

## Status

| Test suite                         | Result |
|------------------------------------|--------|
| Unit (decoder, executor, bus, ...) | **5/5 ctest core ✓**  |
| Time-travel (det, snap, replay,    | **6/6 ✓**             |
| rewind, fw, eth-replay)            |        |
| Firmware integration               | **14/14 ✓**           |

**Throughput:** ~30M IPS hybrid (interpreter + native JIT).
**Snapshot restore:** mean 0.14ms (~263KB blob).
**Rewind across 1M cycles:** mean 0.3ms via O(log n) bsearch on snap index.

Tested firmware:

1. `fib(10) = 55` (Cortex-M0)
2. Bubble-sort + recursive `factorial(6)` (Cortex-M3 -O2 with IT block, STRD)
3. UART `printf` + UDIV/MUL
4. SysTick hardware IRQ counter (5 ticks)
5. MSR/MRS PSP + manual PendSV pending via SCB.ICSR
6. Mini-RTOS — 2-task round-robin scheduler, R4-R11 context switching
7. **FreeRTOS-Kernel V10.6.2 ARM_CM3 port** — 2 tasks with `vTaskDelay`
8. FPU (Cortex-M4F) — `sqrt(3^2+4^2)=5`, area, abs (VLDR, VMUL, VADD, VSQRT,
   VDIV, VSUB, VNEG, VCVT, VMOV-imm)
9. FreeRTOS Queue producer/consumer — `S(1..10) = 55`
10. STM32F103 Blue Pill blink — RCC + GPIOC PC13 + USART1 (real-board fw unmodified)
11. NVIC external IRQ chain (IRQ0, IRQ1) with priorities
12. DSP DFT N=8 with VFMA (Cortex-M4F -O2)
13. Zephyr-lite `k_thread + k_sleep` round-robin
14. Ethernet ICMP echo through MMIO MAC loopback

## Time-travel

The kernel models the emulator state as a pure function of (initial state,
ARM cycles, external events). Three primitives:

```c
tt_t* tt = tt_create(/*stride*/ 5000, /*max_snaps*/ 200);
// run -> tt_on_cycle(...) is O(1) per batch, takes a snap every 5K cycles

tt_rewind(tt, 25000, &cpu, &bus, &p, &jit);          // O(log n) seek
tt_step_back(tt, 1, &cpu, &bus, &p, &jit);           // +/- 1 ARM cycle (whole-insn)
tt_replay(&snap_blob, &log, target_cycle, ...);      // byte-eq across runs
tt_diff(&snap_a, &snap_b, stderr);                   // reg + SRAM range deltas
```

**Event log (`ev_t`, 16 bytes fixed):** UART RX bytes, IRQ injections, ETH frames
(via side-blob store, since ETH payloads exceed 4 bytes).

**Snapshot (`snap_blob_t`, ~263 KB):** full `cpu_t` + 8 peripheral structs +
256 KB SRAM + magic + version + cycle + xor32 checksum. `memcpy`-based, not
COW — Windows-portable, sub-millisecond.

**Replay determinism:** the kernel is rewindable to any cycle and forward-runs
byte-equally as the original. Verified by `test_tt_firmware`: a 50K-cycle
Thumb workload runs three times — REF, then `rewind(25000)+forward`, then
`step_back(10000)+forward` — and all three resulting `snap_blob_t` are
`memcmp == 0`.

## Architecture

```
include/
  core/
    types.h    u8/u16/u32/u64, FORCE_INLINE, LIKELY
    cpu.h      CPU state + flags + IT state + FPU
    fpu.h      32 single-prec regs + FPSCR
    bus.h      region-based memory bus, flat + MMIO
    decoder.h  insn_t + opcode enum
    nvic.h     exc_enter / exc_return
    jit.h      basic-block JIT, hot threshold, native thunk slot
    codegen.h  x86-64 emitter, mmap RWX page pool
    tt.h       ev_t / ev_log_t / tt_t / snap_blob_t / API
    run.h      run_steps_full_g(jit_t*) + run_until_cycle
    gdb.h      RSP stub
  periph/
    uart.h     replay-mode aware UART (rx_q, replay flag)
    systick.h  Cortex-M SysTick at 0xE000E010
    scb.h      SCB ICSR/VTOR/AIRCR at 0xE000ED00
    mpu.h      MPU at 0xE000ED90, 8 regions
    dwt.h      DWT CYCCNT at 0xE0001000
    stm32.h    STM32F103 RCC/GPIO/USART
    eth.h      MMIO MAC + eth_inject_rx (external RX entrypoint)
src/
  core/
    cpu.c      flag computation (NZCV), IT advance
    bus.c      region dispatch + bus_find_flat helper
    decoder.c  Thumb-1 + full Thumb-2 decoder
    executor.c interpretation of all decoded ops
    nvic.c     8-word stack frame, EXC_RETURN, nvic_set_pending_ext (tt hook)
    fpu.c      reset
    jit.c      hot-block trace, jit_t.counters[], jit_reset_counters
    codegen.c  arm-op[] -> x86-64 thunk; native MOV/ADD/SUB/AND/OR/EOR + imm
    tt.c       ev_log_init/append/seek, snap_save/restore, tt_create/rewind/...
    run.c      fetch-decode-execute loop + dcache + run_until_cycle
    gdb.c      Remote Serial Protocol over TCP
  periph/      MMIO callbacks (uart, systick, scb, mpu, dwt, stm32, eth)
tools/main.c   CLI: load .bin, reset vector, run, optional --gdb
tests/         11 ctest suites + test_harness.h
firmware/      14 self-contained ARM firmwares + run_all.sh
```

## Supported ISA

**Thumb-1 (ARMv6-M):** all ~60 instructions

**Thumb-2 (ARMv7-M):**
- Data-proc modified immediate: 16 ops (AND, BIC, ORR, ORN, EOR, ADD, ADC,
  SBC, SUB, RSB, MOV, MVN, TST, TEQ, CMN, CMP)
- Plain immediate: MOVW, MOVT, ADDW, SUBW, ADR
- Data-proc register with shift: same 16 ops
- Memory: LDR/STR (T3, T4), LDRD, STRD, LDM, STM (IA, DB)
- Branches: BL, B.W (cond + uncond)
- Multiply/divide: MUL, MLA, MLS, UMULL, SMULL, UMLAL, SMLAL, UDIV, SDIV
- Bitfield: BFI, BFC, UBFX, SBFX
- Bit ops: CLZ, RBIT
- Register shifts: LSL.W, LSR.W, ASR.W, ROR.W
- Compare-and-branch: CBZ, CBNZ
- IT block (full state machine)
- TBB, TBH (table branch)
- CPS (interrupt enable/disable)
- MSR/MRS (PSP, MSP, PRIMASK, BASEPRI, FAULTMASK, CONTROL, APSR/IPSR/EPSR)

**VFPv4 single-precision (Cortex-M4F):**
- VLDR, VSTR, VLDM, VSTM, VPUSH, VPOP
- VADD, VSUB, VMUL, VDIV, VFMA family
- VSQRT, VNEG, VABS
- VMOV (reg, imm with VFPExpandImm32, R<->F)
- VCMP, VCVT (F<->I)
- VMRS, VMSR (FPSCR)

**System:**
- NVIC: full 240 IRQ lines, SysTick + PendSV via SCB.ICSR.PENDSVSET
- 8-word exception stack frame (R0-R3, R12, LR, PC, xPSR)
- EXC_RETURN: 0xFFFFFFF9 (thread+MSP), 0xFFFFFFFD (thread+PSP),
  0xFFFFFFF1 (handler)
- Thread/Handler modes, MSP/PSP, CONTROL.SPSEL switching
- MPU: 8 regions, AP/SIZE/SRD, PRIVDEFENA fallback
- DWT cycle counter
- Fault escalation (HardFault, MemManage, BusFault, UsageFault)

## Build

Requires CMake 3.15+, MinGW gcc 14+ (or MSVC), arm-none-eabi-gcc 13+ (only
for building firmware/test*; not needed to run the emulator on prebuilt
`.bin` files committed in the repo).

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
bash firmware/run_all.sh
```

Outputs:
- `build/cortex-m.exe` — emulator CLI
- `build/tests/test_*.exe` — 11 ctest unit suites

## Run

```bash
# basic
cortex-m firmware.bin

# with max-instructions limit
cortex-m firmware.bin 1000000

# with GDB stub
cortex-m firmware.bin --gdb=1234
```

In another terminal:

```bash
arm-none-eabi-gdb firmware.elf
(gdb) target remote :1234
(gdb) break main
(gdb) continue
(gdb) info registers
(gdb) step
```

## Memory map

| Range                       | What |
|-----------------------------|------|
| `0x00000000`-`0x00100000`   | Flash (1 MB, RX) |
| `0x20000000`-`0x20040000`   | SRAM (256 KB, RW) |
| `0x40004000`-`0x40004FFF`   | Generic UART (TX -> stdout, replay-mode aware) |
| `0x40010800`-`0x40011000`   | STM32 GPIOA/B/C |
| `0x40013800`-`0x40013BFF`   | STM32 USART1 |
| `0x40021000`-`0x40021400`   | STM32 RCC |
| `0x40028000`-`0x40029000`   | Ethernet MAC (MMIO + ICMP loopback) |
| `0xE0001000`-`0xE00010FF`   | DWT |
| `0xE000E010`-`0xE000E020`   | SysTick |
| `0xE000E100`-`0xE000E4FF`   | NVIC (240 IRQ lines) |
| `0xE000ED00`-`0xE000ED90`   | SCB |
| `0xE000ED90`-`0xE000EDF0`   | MPU |
| `0xE000EDFC`                | DEMCR |

## License

MIT.
