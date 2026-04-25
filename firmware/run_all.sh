#!/usr/bin/env bash
# Build and run all firmware test cases. Verify expected register state.
set -e
cd "$(dirname "$0")"

EMU=../build/cortex-m.exe
PASS=0; FAIL=0

run_test() {
    local name="$1"; shift
    local expect="$1"; shift
    cd "$name"
    bash build.sh > build.log 2>&1
    cd ..
    local out
    out=$("$EMU" "$name/$name.bin" 2>&1)
    if echo "$out" | grep -q "$expect"; then
        echo "PASS $name  ($(echo "$out" | head -1))"
        PASS=$((PASS+1))
    else
        echo "FAIL $name"
        echo "  expected: $expect"
        echo "  output:"
        echo "$out" | sed 's/^/    /'
        FAIL=$((FAIL+1))
    fi
}

# test1: fib(10) = 55 = 0x37 in R0
run_test test1 "R0=00000037"

# test2: bubble sort + sum=40 + fact(6)=720, R0=(40<<16)|720, R1=arr[0]=1, R2=arr[7]=9
run_test test2 "R0=002802d0 R1=00000001 R2=00000009"

# test3: UART printf, UDIV 42/7=6, MUL 42*7=294
run_test test3 "R0=00000006 R1=00000126"

# test4: SysTick+NVIC — 5 ticks via interrupt handler
run_test test4 "R0=00000005"

# test5: MSR/MRS + PSP switch + PendSV handler chain
run_test test5 "R0=00000003 R1=20000200 R2=20000200"

# test6: mini-RTOS — 2 tasks, SysTick+PendSV preemptive round-robin scheduler
run_test test6 "PC=000000aa"

# test7: real FreeRTOS-Kernel V10.6.2 — 2 tasks with vTaskDelay
run_test_freertos() {
    local name="$1"; shift
    local expect="$1"; shift
    cd "$name"
    bash build.sh > build.log 2>&1
    cd ..
    local out
    out=$("$EMU" "$name/$name.bin" 5000000 2>&1)
    if echo "$out" | grep -q "$expect"; then
        echo "PASS $name  ($(echo "$out" | head -1 | tr -d '\r' | cut -c1-50))"
        PASS=$((PASS+1))
    else
        echo "FAIL $name"
        echo "$out" | tail -5 | sed 's/^/    /'
        FAIL=$((FAIL+1))
    fi
}
run_test_freertos test7_freertos "R0=00000014 R1=00000014"

# test8: FPU (Cortex-M4F) — VLDR, VMUL, VADD, VSQRT, VDIV, VSUB, VNEG, VCVT, VMOV imm
run_test test8_fpu "R0=00000005 R1=00000006 R2=00000001"

# test9: FreeRTOS Queue producer/consumer Σ(1..10)=55
run_test_freertos test9_freertos_ipc "R0=00000037 R1=0000000a"

# test10: STM32F103 Blue Pill blink — RCC + GPIOC PC13 + USART1
run_test test10_stm32_blink "R0=00000005"

echo ""
echo "===== $PASS passed, $FAIL failed ====="
[ $FAIL -eq 0 ]
