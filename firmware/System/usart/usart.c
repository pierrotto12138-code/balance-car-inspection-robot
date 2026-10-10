#include "sys.h"
#include "usart.h"	
#ifdef USE_FREERTOS
#include "comm_link.h"      /* printf 与协议报文共用 USART2 → 需要发送互斥 */
#endif
////////////////////////////////////////////////////////////////////////////////// 	 
//如果使用ucos,则包括下面的头文件即可.
#if SYSTEM_SUPPORT_OS
#include "includes.h"					//ucos 使用	  
#endif


//////////////////////////////////////////////////////////////////
// printf 重定向（GCC / newlib 版，2026-10-09 由 B 从 ARMCC 版移植）
//   原版：ARMCC 专有 `#pragma import(__use_no_semihosting)` + `_sys_exit` + `fputc`
//   GCC ：newlib 的 printf 最终走 `_write()`，重定向它即可；
//         `--specs=nosys.specs` 已提供其余系统调用桩，不再需要 _sys_exit。
//   ★ 行为保持一致：Flag_Show==0 走 USART2（蓝牙/APP），否则走 USART1（DataScope 调试口）。
//////////////////////////////////////////////////////////////////
#if 1
#include <stdio.h>
#include <unistd.h>

int _write(int file, char *ptr, int len)
{
	int i;
	(void)file;

#ifdef USE_FREERTOS
	/* ★ P4：printf 与协议报文共用 USART2，必须互斥，否则两条报文会交错成乱码。
	   （CommLink_LockTx 在调度器未运行时是空操作，所以启动阶段也安全。）*/
	CommLink_LockTx();
#endif
	for (i = 0; i < len; i++)
	{
		if (Flag_Show == 0)
		{
			while ((USART2->SR & 0X40) == 0);	// Flag_Show=0 使用串口2
			USART2->DR = (u8)ptr[i];
		}
		else
		{
			while ((USART1->SR & 0X40) == 0);	// Flag_Show!=0 使用串口1
			USART1->DR = (u8)ptr[i];
		}
	}
#ifdef USE_FREERTOS
	CommLink_UnlockTx();
#endif
	return len;
}
#endif
 

//串口1中断服务程序
//bound:波特率
void uart_init(u32 bound){
   //GPIO端口设置
  GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;

	
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA,ENABLE); //使能GPIOA时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1,ENABLE);//使能USART1时钟
 
	//串口1对应引脚复用映射
	GPIO_PinAFConfig(GPIOA,GPIO_PinSource9,GPIO_AF_USART1); //GPIOA9复用为USART1
	GPIO_PinAFConfig(GPIOA,GPIO_PinSource10,GPIO_AF_USART1); //GPIOA10复用为USART1
	
	//USART1端口配置
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10; //GPIOA9与GPIOA10
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;//复用功能
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;	//速度50MHz
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP; //推挽复用输出
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP; //上拉
	GPIO_Init(GPIOA,&GPIO_InitStructure); //初始化PA9，PA10

   //USART1 初始化设置
	USART_InitStructure.USART_BaudRate = bound;//波特率设置
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;//字长为8位数据格式
	USART_InitStructure.USART_StopBits = USART_StopBits_1;//一个停止位
	USART_InitStructure.USART_Parity = USART_Parity_No;//无奇偶校验位
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;//无硬件数据流控制
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;	//收发模式
  USART_Init(USART1, &USART_InitStructure); //初始化串口1
	
  USART_Cmd(USART1, ENABLE);  //使能串口1 
	
#ifdef USE_FREERTOS
	/* ★ B（固件）2026-10-09（决策 45）：把 USART1 变成**有线调试链路** ——
	   与蓝牙（USART2）跑同一套协议，用于实验室调试与 PID 在线调参。
	   中断里同样**只塞环形缓冲**，不调用任何 RTOS API
	   （抢占优先级 0 高于 configMAX_SYSCALL_INTERRUPT_PRIORITY，调 RTOS API 会破坏内核）。*/
	USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
	{
		NVIC_InitTypeDef NVIC_InitStructure;
		NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
		NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
		NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
		NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
		NVIC_Init(&NVIC_InitStructure);
	}
#endif
}


void USART1_IRQHandler(void)                	//串口1中断服务程序
{
#ifdef USE_FREERTOS
	/* 有线链路：与蓝牙侧**完全对称** —— 只推进环形缓冲，不解帧、不调 RTOS API */
	{
		uint32_t sr = USART1->SR;
		if (sr & (USART_SR_RXNE | USART_SR_ORE))
		{
			uint8_t b = (uint8_t)USART1->DR;      /* 读 DR 同时清 RXNE / ORE */
			if (sr & USART_SR_RXNE)
			{
				CommLink_OnRxByte(COMM_LINK_WIRE, b);
			}
			else
			{
				CommLink_NoteOverrun(COMM_LINK_WIRE);
			}
		}
	}
#else
	/* 裸机模式：USART1 留给厂商 DataScope 波形通道（只发不收），故中断不启用 */
#endif
} 


 



