#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Minimal FreeRTOS config for Cortex-M3 in our emulator. */

#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      16000000UL
#define configTICK_RATE_HZ                      ((TickType_t)100)
#define configMAX_PRIORITIES                    4
#define configMINIMAL_STACK_SIZE                ((uint16_t)96)
#define configTOTAL_HEAP_SIZE                   ((size_t)(6 * 1024))
#define configMAX_TASK_NAME_LEN                 8
#define configUSE_TRACE_FACILITY                0
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       0
#define configQUEUE_REGISTRY_SIZE               0
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_COUNTING_SEMAPHORES           0
#define configGENERATE_RUN_TIME_STATS           0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0

/* Hook-related */
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskDelayUntil                 0
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          0

/* Cortex-M3: all 8 priority bits are not implemented; use 4 bits. */
#define configPRIO_BITS                         4
#define configKERNEL_INTERRUPT_PRIORITY         (0xFF)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (0x10)

#define vPortSVCHandler        SVC_Handler
#define xPortPendSVHandler     PendSV_Handler
#define xPortSysTickHandler    SysTick_Handler

#endif
