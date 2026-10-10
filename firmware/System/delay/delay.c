#include "delay.h"
#include "sys.h"
////////////////////////////////////////////////////////////////////////////////// 	 
//如果使用OS,则包括下面的头文件（以ucos为例）即可.
#if SYSTEM_SUPPORT_OS
#include "includes.h"					//支持OS时，使用	  
#endif

//********************************************************************************
// ★ B（固件）2026-10-09 移植（决策 42）：把"非 uCOS"分支的延时改用 **DWT CYCCNT**
//
//   为什么必须改：原版 delay_us()/delay_xms() **直接读写 SysTick**（LOAD/VAL/CTRL），
//   而 FreeRTOS 的 1 ms 节拍也用 SysTick —— 两者会互相破坏：
//   任务里调一次 delay_us(40)，内核节拍就被改写一次，调度时间基准直接乱掉。
//
//   改法：
//     · delay_us()/delay_xms() → DWT CYCCNT 忙等（SysTick 完全让给 FreeRTOS）
//     · delay_ms()             → 调度器已运行时用 vTaskDelay 让出 CPU，否则忙等
//   ★ 三个函数的**语义和调用方式不变**，所以 DHT11/OLED 等驱动一行都不用改。
//   ★ DWT CYCCNT 是 32 位、按 CPU 周期递增；用"无符号差值"比较，天然处理回绕
//     （在 168 MHz 下约 25.6 s 回绕一次，而我们最长只等 20 ms）。
//********************************************************************************

////////////////////////////////////////////////////////////////////////////////// 

static u8  fac_us=0;							//us延时倍乘数			   
static u16 fac_ms=0;							//ms延时倍乘数,在os下,代表每个节拍的ms数
	
#if SYSTEM_SUPPORT_OS							//如果SYSTEM_SUPPORT_OS定义了,说明要支持OS了(不限于UCOS).

#ifdef 	OS_CRITICAL_METHOD						//OS_CRITICAL_METHOD定义了,说明要支持UCOSII				
#define delay_osrunning		OSRunning			//OS是否运行标记,0,不运行;1,在运行
#define delay_ostickspersec	OS_TICKS_PER_SEC	//OS时钟节拍,即每秒调度次数
#define delay_osintnesting 	OSIntNesting		//中断嵌套级别,即中断嵌套次数
#endif

//支持UCOSIII
#ifdef 	CPU_CFG_CRITICAL_METHOD					//CPU_CFG_CRITICAL_METHOD定义了,说明要支持UCOSIII	
#define delay_osrunning		OSRunning			//OS是否运行标记,0,不运行;1,在运行
#define delay_ostickspersec	OSCfg_TickRate_Hz	//OS时钟节拍,即每秒调度次数
#define delay_osintnesting 	OSIntNestingCtr		//中断嵌套级别,即中断嵌套次数
#endif


//us级延时时,关闭任务调度(防止打断us级延迟)
void delay_osschedlock(void)
{
#ifdef CPU_CFG_CRITICAL_METHOD   			//使用UCOSIII
	OS_ERR err; 
	OSSchedLock(&err);						//UCOSIII的方式,禁止调度，防止打断us延时
#else										//否则UCOSII
	OSSchedLock();							//UCOSII的方式,禁止调度，防止打断us延时
#endif
}

//us级延时时,恢复任务调度
void delay_osschedunlock(void)
{	
#ifdef CPU_CFG_CRITICAL_METHOD   			//使用UCOSIII
	OS_ERR err; 
	OSSchedUnlock(&err);					//UCOSIII的方式,恢复调度
#else										//否则UCOSII
	OSSchedUnlock();						//UCOSII的方式,恢复调度
#endif
}

//调用OS自带的延时函数延时
//ticks:延时的节拍数
void delay_ostimedly(u32 ticks)
{
#ifdef CPU_CFG_CRITICAL_METHOD
	OS_ERR err; 
	OSTimeDly(ticks,OS_OPT_TIME_PERIODIC,&err);//UCOSIII延时采用周期模式
#else
	OSTimeDly(ticks);						//UCOSII延时
#endif 
}
 
//systick中断服务函数,使用OS时用到
void SysTick_Handler(void)
{	
	if(delay_osrunning==1)					//OS开始跑了,才执行正常的调度处理
	{
		OSIntEnter();						//进入中断
		OSTimeTick();       				//调用ucos的时钟服务程序               
		OSIntExit();       	 				//触发任务切换软中断
	}
}
#endif
			   
//初始化延迟函数
//SYSCLK:系统时钟频率（MHz）
// ★ B（固件）2026-10-09：不再动 SysTick（交给 FreeRTOS），改为使能 **DWT CYCCNT**。
void delay_init(u8 SYSCLK)
{
#if SYSTEM_SUPPORT_OS 						//如果需要支持OS.
	u32 reload;
#endif
 	SysTick_CLKSourceConfig(SysTick_CLKSource_HCLK_Div8); 
	fac_us=SYSCLK/8;						//不论是否使用OS,fac_us都需要使用
#if SYSTEM_SUPPORT_OS 						//如果需要支持OS.
	reload=SYSCLK/8;						//每秒钟的计数次数 单位为M	   
	reload*=1000000/delay_ostickspersec;	//根据delay_ostickspersec设定溢出时间
											//reload为24位寄存器,最大值:16777216,在168M下,约合0.7989s左右	
	fac_ms=1000/delay_ostickspersec;		//代表OS可以延时的最少单位	   
	SysTick->CTRL|=SysTick_CTRL_TICKINT_Msk;   	//开启SYSTICK中断
	SysTick->LOAD=reload; 					//每1/delay_ostickspersec秒中断一次	
	SysTick->CTRL|=SysTick_CTRL_ENABLE_Msk; 	//开启SYSTICK    
#else
	// ★ DWT 计数的是 **CPU 周期**，所以倍乘数是 SYSCLK 本身（不是 SYSCLK/8）
	fac_us = SYSCLK;
	fac_ms = (u16)fac_us * 1000;

	/* 使能 DWT CYCCNT（Cortex-M4）：DEMCR.TRCENA 必须先置位，否则 DWT 寄存器不可写 */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
}								    

#if SYSTEM_SUPPORT_OS 						//如果需要支持OS.
//延时nus
//nus:要延时的us数.	
//nus:0~204522252(最大值即2^32/fac_us@fac_us=21)	    								   
void delay_us(u32 nus)
{		
	u32 ticks;
	u32 told,tnow,tcnt=0;
	u32 reload=SysTick->LOAD;				//LOAD的值	    	 
	ticks=nus*fac_us; 						//需要的节拍数 
	delay_osschedlock();					//阻止OS调度，防止打断us延时
	told=SysTick->VAL;        				//刚进入时的计数器值
	while(1)
	{
		tnow=SysTick->VAL;	
		if(tnow!=told)
		{	    
			if(tnow<told)tcnt+=told-tnow;	//这里注意一下SYSTICK是一个递减的计数器就可以了.
			else tcnt+=reload-tnow+told;	    
			told=tnow;
			if(tcnt>=ticks)break;			//时间超过/等于要延迟的时间,则退出.
		}  
	};
	delay_osschedunlock();					//恢复OS调度											    
}  
//延时nms
//nms:要延时的ms数
//nms:0~65535
void delay_ms(u16 nms)
{	
	if(delay_osrunning&&delay_osintnesting==0)//如果OS已经在跑了,并且不是在中断里面(中断里面不能任务调度)	    
	{		 
		if(nms>=fac_ms)						//延时的时间大于OS的最少时间周期 
		{ 
   			delay_ostimedly(nms/fac_ms);	//OS延时
		}
		nms%=fac_ms;						//OS已经无法提供这么小的延时了,采用普通方式延时    
	}
	delay_us((u32)(nms*1000));				//普通方式延时
}
#else  //不用ucos时：★ 已改为 DWT CYCCNT（把 SysTick 让给 FreeRTOS，见文件顶部说明）

// 延时 nus 微秒（忙等，基于 DWT 周期计数器）
// nus 上限：2^32 / fac_us @fac_us=168 → 约 25.5 s，远大于实际需要
void delay_us(u32 nus)
{
	u32 start;
	u32 ticks;

	if (nus == 0)
	{
		return;
	}
	/* DWT 已在 delay_init() 里使能；若不可用则退化为空操作（不阻塞系统）*/
	if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0)
	{
		return;
	}

	ticks = nus * fac_us;                    // 需要的 CPU 周期数
	start = DWT->CYCCNT;
	while ((u32)(DWT->CYCCNT - start) < ticks)
	{
		/* 忙等 */
	}
}

// 延时 nms 毫秒（忙等版，供"调度器未运行"时使用）
// 注：不再依赖 SysTick 的 24 位限制，用 delay_us(1000) 循环实现
void delay_xms(u16 nms)
{
	while (nms--)
	{
		delay_us(1000);
	}
}

// 延时 nms 毫秒
//  ★ 调度器跑起来之后用 vTaskDelay 让出 CPU（期间其它任务照常运行）；
//    启动阶段（调度器未运行）或纯裸机构建则忙等。
void delay_ms(u16 nms)
{
#if defined(USE_FREERTOS)
	extern int App_SchedulerRunning(void);
	if (App_SchedulerRunning())
	{
		extern void App_DelayMs(u16 nms);
		App_DelayMs(nms);                    // 由 App 层转成 vTaskDelay（避免本文件依赖 FreeRTOS 头）
		return;
	}
#endif
	while (nms--)
	{
		delay_us(1000);
	}
}
#endif
			 



































