#include "board_state.h"

#include "sys.h"
#include "motor.h"      /* PWMA/PWMB：TIM1 的 CCR 宏（真实输出）*/
#include "control.h"    /* Balance_Pwm / Velocity_Pwm / Turn_Pwm 的 extern 声明 */
/* ★ 2026-10-15：必须包含 app_config.h —— 本文件用了 `#if CFG_IMU_LR_FROM_PITCH`，
   宏不可见时 `#if` 会**静默取 0**（于是悄悄走错分支、取错 IMU 轴）。
   这正是铁律 35 的原型坑：凡在 .c 里用 `#if CFG_*`，该 .c 必须能看到定义它的头文件。
   （`-Wundef` 也能抓住它：`make strict` 当时就是这么报出来的。）*/
#include "app_config.h"
#include "MPU6050.h"    /* Pitch / Roll / gyro[] 的厂商 extern 声明 */
/* 这两个量没有集中声明处（App_MotorDisable 定义在 app_tasks.c、g_pwmApplied 定义在
   control.c），这里显式 extern，类型与定义处一致（volatile u8）。*/
extern volatile u8 App_MotorDisable;
extern volatile u8 g_pwmApplied;
/* 厂商运动方向标志（定义在 User/main.c）。这里显式 extern，因为没有任何头集中声明它们。*/
#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#endif

/*
 * 见 board_state.h 的说明：本文件是 App 层访问厂商全局量的**唯一出口**。
 *   · 只做"搬运"，不含任何业务判断（阈值、锁存、优先级都在 App 层）；
 *   · 临界区只在 `Board_ReadRaw()` 用一次，保证一组量来自同一拍。
 */

void Board_ReadRaw(BoardRawState *out)
{
	if (out == 0)
	{
		return;
	}

#ifdef USE_FREERTOS
	taskENTER_CRITICAL();
#endif
	out->angle      = Angle_Balance;
	out->gyro       = Gyro_Balance;
	out->gyroTurn   = Gyro_Turn;
	/* ★ 2026-10-15：左右倾角/角速度（供 0x10 的 roll / gy 字段）。
	   厂商 DMP 只算了 Pitch/Roll 两个角（**没有偏航角**），而平衡角用的是 `-Roll`
	   ⇒ Roll 是前后倾、**Pitch 即左右倾**；角速度与 `gyro[0]=前后 / gyro[2]=偏航` 正交 ⇒ 用 `gyro[1]`。
	   `Pitch`/`Roll`/`gyro[]` 都是厂商全局量（MPU6050.c 定义、MPU6050.h 已 extern），本层直读。*/
#if CFG_IMU_LR_FROM_PITCH
	out->tiltLR     = Pitch;
#else
	out->tiltLR     = Roll;      /* 若真机实测该轴不对，改 app_config.h 的开关即可 */
#endif
	out->gyroLR     = (float)gyro[1];
	out->encLeft    = (int32_t)Encoder_Left;
	out->encRight   = (int32_t)Encoder_Right;
	out->pwmLeft    = (int32_t)Moto1;
	out->pwmRight   = (int32_t)Moto2;
	out->voltage    = (int32_t)Voltage;
	out->distance   = (uint32_t)Distance;
	out->vendorStop = (uint8_t)Flag_Stop;
	out->showFlag   = (uint8_t)Flag_Show;
#ifdef USE_FREERTOS
	taskEXIT_CRITICAL();
#endif
}

uint8_t Board_ReadVendorStop(void)
{
	return (uint8_t)Flag_Stop;
}

void Board_WriteVendorStop(uint8_t on)
{
#ifdef USE_FREERTOS
	taskENTER_CRITICAL();
#endif
	Flag_Stop = (u8)(on ? 1 : 0);
#ifdef USE_FREERTOS
	taskEXIT_CRITICAL();
#endif
}

void Board_SyncPidParams(float balanceKp, float balanceKd,
                         float velocityKp, float velocityKi,
                         float zhongzhi, float turnKp, float turnKd)
{
	Balance_Kp  = balanceKp;
	Balance_Kd  = balanceKd;
	Velocity_Kp = velocityKp;
	Velocity_Ki = velocityKi;
	Zhongzhi    = zhongzhi;
	Turn_Kp     = turnKp;
	Turn_Kd     = turnKd;
}

void Board_CapturePidParams(float *balanceKp, float *balanceKd,
                            float *velocityKp, float *velocityKi,
                            float *zhongzhi, float *turnKp, float *turnKd)
{
	if (balanceKp != 0)  { *balanceKp  = Balance_Kp;  }
	if (balanceKd != 0)  { *balanceKd  = Balance_Kd;  }
	if (velocityKp != 0) { *velocityKp = Velocity_Kp; }
	if (velocityKi != 0) { *velocityKi = Velocity_Ki; }
	if (zhongzhi != 0)   { *zhongzhi   = Zhongzhi;    }
	if (turnKp != 0)     { *turnKp     = Turn_Kp;     }
	if (turnKd != 0)     { *turnKd     = Turn_Kd;     }
}

void Board_ReadControlInternals(BoardControlInternals *out)
{
	if (out == 0)
	{
		return;
	}
#ifdef USE_FREERTOS
	taskENTER_CRITICAL();
#endif
	out->balancePwm   = (int16_t)Balance_Pwm;
	out->velocityPwm  = (int16_t)Velocity_Pwm;
	out->turnPwm      = (int16_t)Turn_Pwm;
	out->moto1        = (int16_t)Moto1;
	out->moto2        = (int16_t)Moto2;
	out->ccrA         = (uint16_t)PWMA;      /* 真实写进寄存器的值 */
	out->ccrB         = (uint16_t)PWMB;
	out->encLeft      = (int16_t)Encoder_Left;
	out->encRight     = (int16_t)Encoder_Right;
	out->flagStop     = (uint8_t)Flag_Stop;
	out->motorDisable = (uint8_t)App_MotorDisable;
	out->pwmApplied   = (uint8_t)g_pwmApplied;
#ifdef USE_FREERTOS
	taskEXIT_CRITICAL();
#endif
}

void Board_WriteMotion(uint8_t dirFlags)
{
#ifdef USE_FREERTOS
	taskENTER_CRITICAL();
#endif
	/* 先全清再置位：与厂商 APP 解析的语义一致（同一时刻最多一个前进/后退、一个左/右）*/
	Flag_Qian  = (u8)((dirFlags & 0x01u) ? 1 : 0);
	Flag_Hou   = (u8)((dirFlags & 0x02u) ? 1 : 0);
	Flag_Left  = (u8)((dirFlags & 0x04u) ? 1 : 0);
	Flag_Right = (u8)((dirFlags & 0x08u) ? 1 : 0);
#ifdef USE_FREERTOS
	taskEXIT_CRITICAL();
#endif
}

void Board_StopMotion(void)
{
	Board_WriteMotion(0);
}

void Board_LedSet(uint8_t on)
{
	LED = (on != 0) ? 0 : 1;        /* ★ 低电平点亮：on=1 → 引脚置 0 */
}
