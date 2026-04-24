/* FreeRTOS demo: 2 tasks printing different chars, one higher priority. */

#include "FreeRTOS.h"
#include "task.h"

#define UART_DR (*(volatile unsigned*)0x40004000)

static void putch(char c) { UART_DR = (unsigned)c; }

static volatile unsigned count_a = 0;
static volatile unsigned count_b = 0;

static void taskA(void* p) {
    (void)p;
    for (;;) {
        putch('A');
        count_a++;
        if (count_a >= 20 && count_b >= 20) {
            __asm__ volatile (
                "mov r0, %0\n"
                "mov r1, %1\n"
                ".short 0xDEFE\n"
                :: "r"(count_a), "r"(count_b) : "r0","r1"
            );
        }
        vTaskDelay(1);
    }
}

static void taskB(void* p) {
    (void)p;
    for (;;) {
        putch('B');
        count_b++;
        vTaskDelay(1);
    }
}

/* FreeRTOS hook for failed malloc — halt. */
void vApplicationMallocFailedHook(void) {
    __asm__ volatile (".short 0xDEFE");
}

int main(void) {
    xTaskCreate(taskA, "A", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(taskB, "B", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    vTaskStartScheduler();
    for (;;) {}
}
