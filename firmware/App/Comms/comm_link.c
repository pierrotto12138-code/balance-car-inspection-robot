#include "comm_link.h"
#include "ringbuf.h"
#include "app_config.h"
#include "sys.h"

#ifdef USE_FREERTOS
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#endif

/* 见 comm_link.h 的分工说明。本文件是**硬件相关**的，不参与 PC 原生测试。 */

typedef struct
{
	USART_TypeDef *uart;
	RingBuf        rx;
	volatile uint32_t rxBytes;
	volatile uint32_t overruns;
} CommLinkCtx;

static CommLinkCtx s_link[COMM_LINK_COUNT];

#ifdef USE_FREERTOS
static SemaphoreHandle_t s_txMutex;
#endif

/* ------------------------------------------------------------------ 初始化 */

void CommLink_Init(void)
{
	uint8_t i;

	for (i = 0; i < (uint8_t)COMM_LINK_COUNT; i++)
	{
		RingBuf_Init(&s_link[i].rx);
		s_link[i].rxBytes = 0;
		s_link[i].overruns = 0;
		s_link[i].uart = 0;
	}
	s_link[COMM_LINK_BT].uart = USART2;
	s_link[COMM_LINK_WIRE].uart = USART1;

#ifdef USE_FREERTOS
	if (s_txMutex == 0)
	{
		s_txMutex = xSemaphoreCreateMutex();
	}
#endif
}

/* ------------------------------------------------------------------ 中断侧 */

void CommLink_OnRxByte(CommLinkId link, uint8_t b)
{
	if (link >= COMM_LINK_COUNT)
	{
		return;
	}
	/* 只进环形缓冲；满时 RingBuf_Push 内部丢新字节并计数（见 ringbuf.c）*/
	(void)RingBuf_Push(&s_link[link].rx, b);
	s_link[link].rxBytes++;
}

void CommLink_NoteOverrun(CommLinkId link)
{
	if (link < COMM_LINK_COUNT)
	{
		s_link[link].overruns++;
	}
}

/* ------------------------------------------------------------------ 任务侧 */

uint16_t CommLink_Read(CommLinkId link, uint8_t *out, uint16_t maxLen)
{
	if (link >= COMM_LINK_COUNT)
	{
		return 0;
	}
	return RingBuf_PopBuf(&s_link[link].rx, out, maxLen);
}

#ifdef USE_FREERTOS
void CommLink_LockTx(void)
{
	/* 调度器未跑（启动阶段）时不加锁，避免在调度器未运行前使用信号量 */
	if (s_txMutex != 0 && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		(void)xSemaphoreTake(s_txMutex, portMAX_DELAY);
	}
}

void CommLink_UnlockTx(void)
{
	if (s_txMutex != 0 && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		(void)xSemaphoreGive(s_txMutex);
	}
}

uint32_t CommLink_NowMs(void)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
	}
	return 0;
}
#else
void CommLink_LockTx(void) { }
void CommLink_UnlockTx(void) { }
uint32_t CommLink_NowMs(void) { return 0; }
#endif

/* ------------------------------------------------------------------ 发送 */

uint16_t CommLink_Send(CommLinkId link, const uint8_t *data, uint16_t len)
{
	USART_TypeDef *u;
	uint16_t i;

	if (link >= COMM_LINK_COUNT || data == 0 || len == 0)
	{
		return 0;
	}
	u = s_link[link].uart;
	if (u == 0)
	{
		return 0;
	}

	/* ★ 与 printf 共用同一物理串口：必须互斥，否则两条报文会交错成乱码 */
	CommLink_LockTx();

	for (i = 0; i < len; i++)
	{
		uint32_t guard = 0;
		while ((u->SR & USART_SR_TXE) == 0)          /* 等发送寄存器空 */
		{
			if (++guard > CFG_TX_TIMEOUT_LOOPS)
			{
				CommLink_UnlockTx();                  /* 串口异常：宁可丢帧也不挂住任务 */
				return i;
			}
		}
		u->DR = (uint16_t)data[i];
	}
	{
		uint32_t guard = 0;
		while ((u->SR & USART_SR_TC) == 0)           /* 等最后一个字节发完，避免帧粘连 */
		{
			if (++guard > CFG_TX_TIMEOUT_LOOPS)
			{
				break;
			}
		}
	}

	CommLink_UnlockTx();
	return len;
}

void CommLink_SendAll(const uint8_t *data, uint16_t len)
{
	(void)CommLink_Send(COMM_LINK_BT, data, len);
	(void)CommLink_Send(COMM_LINK_WIRE, data, len);
}

/* ------------------------------------------------------------------ 诊断 */

uint32_t CommLink_RxBytes(CommLinkId link)
{
	return (link < COMM_LINK_COUNT) ? s_link[link].rxBytes : 0;
}

uint32_t CommLink_RxDropped(CommLinkId link)
{
	return (link < COMM_LINK_COUNT) ? s_link[link].rx.dropped : 0;
}

uint32_t CommLink_Overruns(CommLinkId link)
{
	return (link < COMM_LINK_COUNT) ? s_link[link].overruns : 0;
}
