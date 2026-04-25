# Requirements: meridian (cortex-m-emu)

**Defined:** 2026-04-26
**Core Value:** f(state, time, events) -> state' deterministic, fast, snapshotable, reversible

## v1.0 Requirements (Validated, shipped)

See MILESTONES.md. 62 tests green.

## v2.0 Requirements

### Time-Travel Kernel

- [ ] **TT-01**: emulator runs with no hidden non-determinism (no rand, no real-time clock inside f)
- [ ] **TT-02**: all I/O events (UART RX, IRQ, ETH frame) recorded with cycle stamp into event log
- [ ] **TT-03**: snapshot(state) returns serializable blob, restore(blob) reproduces state byte-equal
- [ ] **TT-04**: copy-on-write snapshots (mmap-based), sub-100ms restore for 256KB SRAM
- [ ] **TT-05**: replay(snap, log, target_cycle) -> state'(target_cycle), byte-equal across runs
- [ ] **TT-06**: rewind(target_cycle) seeks via nearest snapshot + replay, O(log n)
- [ ] **TT-07**: step_back(N) walks N ARM cycles backward through current execution
- [ ] **TT-08**: diff(snap_a, snap_b) reports register and memory deltas

### JIT Performance

- [ ] **JIT-01**: direct block chaining: terminator emits jmp rel32 to next TB
- [ ] **JIT-02**: native codegen for LDR/STR (immediate + register variants) via helper call
- [ ] **JIT-03**: native flag-setter for CMP / ADDS / SUBS via x86 LEA + flag map
- [ ] **JIT-04**: native conditional branch (B.cond) via x86 jcc
- [ ] **JIT-05**: TB cache LRU eviction when buffer fills
- [ ] **JIT-06**: benchmark passes: FreeRTOS 5M instructions in under 50ms (target 100M+ IPS)

### WASM / Web IDE

- [ ] **WEB-01**: emulator core compiles via emscripten to .wasm + .js loader
- [ ] **WEB-02**: GDB stub replaced by postMessage-based debug bridge in WASM build
- [ ] **WEB-03**: web IDE: Monaco editor for C, build button, run button, register pane
- [ ] **WEB-04**: web IDE shows UART output as terminal, GPIO state as LEDs
- [ ] **WEB-05**: at least 5 lessons (hello UART, blink, fib, FreeRTOS task, FFT) load and run
- [ ] **WEB-06**: auto-grader compares UART output / register state to expected, marks pass/fail

### Python API + CI Runner

- [ ] **CI-01**: Python module: import meridian; b = meridian.Board("stm32f407"); b.flash(file)
- [ ] **CI-02**: b.run(timeout_ms) returns exit cause (halt / timeout / fault)
- [ ] **CI-03**: b.uart.output() / b.gpio[port][pin].value / b.cpu.r[N] readable from Python
- [ ] **CI-04**: pytest plugin: pytest tests/ runs every test_*.py against firmware
- [ ] **CI-05**: GitHub Action: meridian/runner@v1 with board+firmware+test inputs
- [ ] **CI-06**: docker image meridian/runner:latest under 50MB, single binary inside

### Landing & Distribution

- [ ] **DIST-01**: meridian.dev landing page with demo, docs, install
- [ ] **DIST-02**: README.md final version with architecture diagram + roadmap
- [ ] **DIST-03**: GitHub repo public with MIT license + CI workflow + release tags
- [ ] **DIST-04**: single-binary releases for Linux/macOS/Windows (CI builds)

## v3.0 Requirements (Future)

### Probe (security research)

- **PROBE-01**: integrate angr via Python bridge for symbolic exec
- **PROBE-02**: coverage-guided fuzzer with libFuzzer ABI
- **PROBE-03**: vendor ROM database for STM32 BootROM, Nordic SoftDevice
- **PROBE-04**: time-travel GUI (Tauri/Electron) with rewind slider

### Advanced ISA

- **ISA-01**: ARMv8-M TrustZone (Cortex-M33)
- **ISA-02**: VFP double-precision
- **ISA-03**: Helium / MVE for Cortex-M55
- **ISA-04**: Cortex-A (Linux user-mode)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Multi-core SMP | Cortex-M is uniprocessor by spec |
| ARMv8-M TZ in v2.0 | niche, defer to v3.0 |
| Cortex-A in v2.0 | different target, separate product |
| Full symbolic exec in v2.0 | v3.0 probe scope |
| VS Code extension | after web IDE proves demand |
| GUI desktop app for probe | v3.0 |
| Real network stack | mock loopback enough for ICMP demo |

## Traceability

Filled by roadmapper. v2.0 requirements: 26 total.

| Requirement | Phase | Status |
|-------------|-------|--------|
| TT-01 .. TT-08 | Phase 13 | Pending |
| JIT-01 .. JIT-06 | Phase 14 | Pending |
| WEB-01 .. WEB-06 | Phase 15 | Pending |
| CI-01 .. CI-06 | Phase 16 | Pending |
| DIST-01 .. DIST-04 | Phase 17 | Pending |

**Coverage:** 26 / 26 mapped.

---
*Defined: 2026-04-26*
