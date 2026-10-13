#ifndef APP_BOARD_BOARD_STATE_H
#define APP_BOARD_BOARD_STATE_H

/*
 * 板级状态适配层 —— **App 层访问厂商全局量的唯一出口**
 *
 * 为什么要有这一层（2026-10-13 P2 结构整改）：
 *   · 厂商代码（`main.c` / `control.c` / `show.c`…）把状态放在一堆全局量里
 *     （`Angle_Balance` / `Moto1,2` / `Voltage` / `Flag_Stop` / PID 增益…）。
 *   · 如果 App 的每个模块都各自 `extern` 一遍，就会出现"同一个量在 N 个文件里
 *     各自声明"——改一处漏一处（`make strict` 的 `-Wredundant-decls` 正是抓这个）。
 *   · 所以约定：**业务代码只 include 本头文件**，由 `board_state.c` 统一去碰厂商全局量。
 *     以后换板 / 换厂商实现，只需要改 `board_state.c` 一个文件。
 *
 * 线程与原子性约定：
 *   · "控制中断写、任务读"的 32 位对齐量，单次读写本身是原子的；
 *     `Board_ReadRaw()` 额外用**短临界区**把一组相关量取成"同一拍"的一致快照
 *     （临界区由本层内部管理，调用方不必关心）。
 *   · 这些数据**只用于上报/显示**，不参与控制 —— 所以不需要更强的同步。
 *
 * ★ 本头文件刻意只依赖 `<stdint.h>`：`board_state.c` 里的厂商类型（`u8`/`int`）
 *   不会泄漏到 App 层，业务代码用标准整型。
 */

#include <stdint.h>

/* 一帧"厂商原始状态"快照（单位沿用厂商约定，见各字段注释）*/
typedef struct
{
	float    angle;        /* 平衡倾角（°，厂商 Angle_Balance）*/
	float    gyro;         /* 平衡角速度（厂商 Gyro_Balance）*/
	float    gyroTurn;     /* 转向角速度（厂商 Gyro_Turn）*/
	int32_t  encLeft;      /* 左编码器计数（10 ms 周期增量）*/
	int32_t  encRight;     /* 右编码器计数 */
	int32_t  pwmLeft;      /* 左电机 PWM（含符号）*/
	int32_t  pwmRight;     /* 右电机 PWM（含符号）*/
	int32_t  voltage;      /* 电池电压（0.01 V）*/
	uint32_t distance;     /* 超声波距离（厂商原始单位）*/
	uint8_t  vendorStop;   /* 厂商 Flag_Stop：1 = 电机被保护/停 */
	uint8_t  showFlag;     /* 厂商 Flag_Show：0 = 用 OLED/APP，非 0 = 用 DataScope */
} BoardRawState;

/* 取一帧厂商原始状态（内部短临界区；任务上下文调用）*/
void Board_ReadRaw(BoardRawState *out);

/* ---- 厂商"停止标志"的读写（使能真相的**唯一出口**）----
 * ★ 权威关系（决策 47 / 编码规范 §二）：`App_MotorDisable`（任务侧锁存）是唯一权威，
 *   厂商 `Flag_Stop` 只是**执行器** —— 中断每拍据权威强制置位它。
 *   本层只负责"怎么碰那个变量"，不负责"什么时候碰"（那是 App 的语义）。 */
uint8_t Board_ReadVendorStop(void);
void    Board_WriteVendorStop(uint8_t on);

/* ---- 板载 LED（PB13，厂商位带宏 `LED`）----
 * 为什么也放这一层：`LED` 是厂商 `led.h` 里的**位带宏**，业务代码直接用它会
 * 把"板级细节"漏进任务层（P2 结构整改的最后一块）；
 * 而且 LED 是**低电平点亮**，容易接反 —— 这里用 `on` 的语义包一层，读代码的人不必记极性。
 * `Led_Flash()` 的闪烁节奏仍由厂商 `control.c` 的中断负责，这里只处理"常亮/常灭"这种状态。*/
void Board_LedSet(uint8_t on);

/* ---- PID 参数：参数表 ↔ 厂商全局量 的双向同步 ----
 * `Set*` 写进厂商全局量后，控制中断下一拍就能用到（对齐 float 写是原子的）。*/
void Board_SyncPidParams(float balanceKp, float balanceKd,
                         float velocityKp, float velocityKi,
                         float zhongzhi, float turnKp, float turnKd);
void Board_CapturePidParams(float *balanceKp, float *balanceKd,
                            float *velocityKp, float *velocityKi,
                            float *zhongzhi, float *turnKp, float *turnKd);

#endif /* APP_BOARD_BOARD_STATE_H */
