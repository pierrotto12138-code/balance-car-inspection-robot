#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/*
 * 应用层**集中配置**（B，2026-10-09）
 *
 * 为什么单独放一个头文件：
 *   固件里有很多"到货后要按实测调整"的量（轮径、编码器线数、告警阈值、周期…）。
 *   把它们集中在一处，联调时只改这一个文件，不用在几千行里翻；
 *   同时它也是**给上位机/C 的接口说明**：这些量的含义与单位都在注释里。
 *
 * ★ 标 [待实测] 的项必须在板子到货后按实际测量值修正。
 */

/* ============================================================================
 * 一、链路
 * ==========================================================================*/
#define CFG_BAUD_BLUETOOTH   115200u   /* USART2（蓝牙 SPP，协议主链路）*/
#define CFG_BAUD_WIRE        115200u   /* USART1（有线调试链路，同协议；USB-TTL 接 PA9/PA10）*/
#define CFG_TX_TIMEOUT_LOOPS 2000000u  /* 串口发送等待上限（防死等）*/

/* ============================================================================
 * 二、整车标定 [待实测]
 * ==========================================================================*/
#define CFG_ENCODER_COUNTS_PER_REV  1560   /* 13 线霍尔四倍频后每圈计数（决策记录 §6）[待实测] */
#define CFG_WHEEL_DIAMETER_MM       67     /* 轮径 mm —— **必须实测**（决定 mm/s 是否可信）[待实测] */
#define CFG_ENCODER_PERIOD_MS       10     /* 编码器读取周期（控制中断 10 ms 分支）*/

/* 编码器计数 → 轮速 mm/s 的换算系数：(周长 mm / 每圈计数) / 周期 s */
#define CFG_MM_PER_COUNT  (3.14159265f * (float)CFG_WHEEL_DIAMETER_MM / (float)CFG_ENCODER_COUNTS_PER_REV)
#define CFG_MS_PER_ENCODER_PERIOD ((float)CFG_ENCODER_PERIOD_MS / 1000.0f)

/* ============================================================================
 * 三、任务周期与栈（1 字 = 4 B）
 * ==========================================================================*/
#define CFG_COMM_TICK_MS       20    /* 通信任务基准节拍 */
#define CFG_COMM_TICK_TUNE_MS   5    /* 调参观测模式下的节拍（更细的定时粒度）*/
#define CFG_GAS_POLL_MS        50
#define CFG_STATE_TICK_MS     100
#define CFG_UI_TICK_MS        100

#define CFG_STACK_COMM   384
#define CFG_STACK_GAS    384
#define CFG_STACK_STATE  320
#define CFG_STACK_UI     512

/* 上行周期 */
#define CFG_TX_TELEMETRY_MS    50    /* 0x10/0x11/0x12：20 Hz */
#define CFG_TX_TELEMETRY_TUNE_MS 10  /* 调参观测模式：100 Hz（0x10/0x11）*/
#define CFG_TX_HEARTBEAT_MS   200    /* 0x1F：5 Hz */
#define CFG_TX_EVENT_IMMEDIATE  1    /* 事件触发即发 */

/* ============================================================================
 * 四、采样与研判
 * ==========================================================================*/
#define CFG_GAS_ADC_AVG          8     /* 每次采样做几次 ADC 平均 */
#define CFG_GAS_CALIB_SAMPLES   60     /* 清洁空气标定累加次数（预热期 2 Hz → 约 30 s）*/
#define CFG_DHT_PERIOD_MS     1000     /* DHT11 最快 1 Hz */

/* 协议 §4.1 的默认巡检参数（可被 0x23 覆盖）*/
#define CFG_DEF_SAMPLE_MS        1000
#define CFG_DEF_MQ2_THRESH_X10    300   /* 30.0 ppm（估计值）[待标定] */
#define CFG_DEF_MQ135_THRESH_X10  500   /* 50.0 ppm（估计值）[待标定] */
#define CFG_DEF_ADAPTIVE            1
#define CFG_DEF_TEMP_COMP           0   /* 温湿度补偿默认关（决策 39）*/
#define CFG_DEF_CONFIRM_FRAMES      3

/* ============================================================================
 * 五、状态机 / 安全（与协议 §5 一致）
 * ==========================================================================*/
#define CFG_WARMUP_MS        180000u   /* 3 min 运行时预热 [待实测] */
#define CFG_DWELL_MS           5000u   /* 异常驻留加密采样 */
#define CFG_LINK_TIMEOUT_MS     500u   /* 协议 §5：500 ms 未收到下行帧 → 安全停车 */
#define CFG_TASK_TIMEOUT_MS 1800000u   /* 30 min 任务总超时 */
#define CFG_TOTAL_WAYPOINTS       4    /* 巡检点数量 [待实测] */

/* ============================================================================
 * 六、PID 默认值（与厂商基线一致；未被 0x24 修改/未从 Flash 载入时生效）
 * ==========================================================================*/
#define CFG_PID_BALANCE_KP    300.0f
#define CFG_PID_BALANCE_KD      1.0f
#define CFG_PID_VELOCITY_KP    80.0f
#define CFG_PID_VELOCITY_KI     0.4f
#define CFG_PID_ZHONGZHI        1.0f
#define CFG_PID_TURN_KP        52.0f   /* 厂商 turn() 内局部常量 */
#define CFG_PID_TURN_KD         0.5f   /* 厂商：仅前后运动时取 0.5 */

/* ============================================================================
 * 七、参数持久化（Flash）
 * ==========================================================================*/
#define CFG_PARAM_FLASH_ADDR  0x08040000u  /* 扇区 6（128 KB）起始；本工程代码仅 60 KB，不会自撞 */
#define CFG_PARAM_MAGIC       0x50494431u  /* 'PID1'，用于判断 Flash 里是否已有有效参数 */

#endif /* __APP_CONFIG_H */
