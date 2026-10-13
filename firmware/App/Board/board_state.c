#include "board_state.h"

#include "sys.h"            /* 厂商全局量的**统一声明处**（见 System/sys/sys.h）*/
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

void Board_LedSet(uint8_t on)
{
	LED = (on != 0) ? 0 : 1;        /* ★ 低电平点亮：on=1 → 引脚置 0 */
}
