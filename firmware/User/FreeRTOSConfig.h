#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*
 * FreeRTOS 配置（STM32F405RGT6 / Cortex-M4F / 168 MHz）
 *   内核：官方 FreeRTOS-Kernel V11.1.0（MIT），见 ../Middlewares/FreeRTOS/VERSION.txt
 *
 * ★ 架构约定（决策 8，见 docs/design_firmware/architecture.md）：
 *     平衡控制**仍然跑在 MPU6050 中断里**（5 ms / 10 ms 节拍不变），
 *     FreeRTOS 只承载"可以并发等待"的部分（气体采集、通信、状态机、界面）。
 *     所以本配置里**中断优先级**必须谨慎：
 *       · 控制中断 EXTI15_10 的抢占优先级要 **高于** configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
 *         （数值更小）→ 它**不能**调用任何 FreeRTOS API，从而绝不会被调度器延迟；
 *       · 需要调 FreeRTOS API 的中断（如串口）优先级数值必须 ≥ 5。
 */

/* ---------- 调度器基本配置 ---------- */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1   /* CM4 有 CLZ 指令，用硬件选任务 */
#define configUSE_TICKLESS_IDLE                 0   /* 巡检车不省这点电，避免时序复杂化 */
#define configCPU_CLOCK_HZ                      ( 168000000UL )
#define configTICK_RATE_HZ                      ( 1000 )        /* 1 ms 节拍 */
#define configMAX_PRIORITIES                    ( 8 )
#define configMINIMAL_STACK_SIZE                ( 128 )         /* 单位：字（4 B） */
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 24 * 1024 ) )  /* heap_4 */
#define configMAX_TASK_NAME_LEN                 ( 12 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0   /* 不用 newlib 的重入结构，省 RAM */
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0

/* ---------- 钩子（出错时不要静默） ---------- */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* ---------- 软件定时器 ---------- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            ( configMINIMAL_STACK_SIZE * 2 )

/* ---------- 运行时统计 / 调试 ---------- */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* ---------- 中断优先级（Cortex-M4，4 位优先级，见 stm32f4xx.h 的 NVIC_PriorityGroup） ---------- */
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/* 断言：出错时关中断并停住（挂调试器即可看到出错位置） */
#define configASSERT( x )                                            \
    if( ( x ) == 0 )                                                 \
    {                                                                \
        taskDISABLE_INTERRUPTS();                                    \
        for( ;; );                                                   \
    }

/* ---------- 可选 API ---------- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1

/* ---------- 中断向量绑定：port.c 提供这三个函数 ---------- */
#define xPortPendSVHandler                      PendSV_Handler
#define xPortSysTickHandler                     SysTick_Handler
#define vPortSVCHandler                         SVC_Handler

#endif /* FREERTOS_CONFIG_H */
