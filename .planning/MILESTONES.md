# Milestones

## v1.0 — Core Emulator (shipped)

12 phases, 62 tests green.

- p1-p2: ISA Thumb-1 + Thumb-2 (cortex-m3 -O2 works)
- p3-p4: NVIC, SysTick, UART, multiply/divide
- p5-p6: MSR/MRS, PSP, mini-RTOS round-robin
- p7: FreeRTOS-Kernel V10.6.2 unmodified
- p8: MPU + VFP single-prec + icache
- p9: GDB stub TCP RSP, STM32 mocks, DWT
- p10: Full NVIC 240 lines, VFMA family, VPUSH/VPOP
- p11: Fault escalation, Zephyr-lite, Ethernet ICMP, jit-lite
- p12: Real x86-64 JIT codegen, mmap RWX, ~30M IPS

## v2.0 — Time-Travel + Product Platform (active)

Goal: turn the engine into a usable product.
Three faces on one deterministic core: meridian-ci, meridian-learn, meridian-probe.
