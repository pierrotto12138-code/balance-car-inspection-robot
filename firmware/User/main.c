#include "stm32f4xx.h"
#include "sys.h"  
#ifdef USE_FREERTOS
#include "app_tasks.h"
#include "app_config.h"
#endif

u8 Way_Angle=2;                             //获取角度的算法，1：四元数  2：卡尔曼  3：互补滤波 
u8 Flag_Qian,Flag_Hou,Flag_Left,Flag_Right,Flag_sudu=2; //蓝牙遥控相关的变量
u8 Flag_Stop=1,Flag_Show=0,Flag_Hover=0;    //停止标志位和 显示标志位 默认停止 显示打开
int Encoder_Left,Encoder_Right;             //左右编码器的脉冲计数
int Moto1,Moto2;                            //电机PWM变量 应是Motor的 向Moto致敬	
int Temperature;                            //显示温度
int Voltage;                                //电池电压采样相关的变量
float Angle_Balance,Gyro_Balance,Gyro_Turn; //平衡倾角 平衡陀螺仪 转向陀螺仪
float Show_Data_Mb;                         //全局显示变量，用于显示需要查看的数据
u32 Distance;                               //超声波测距
u8 delay_50,delay_flag,Bi_zhang=0,PID_Send,Flash_Send; //延时和调参等变量
float Acceleration_Z;                       //Z轴加速度计  
float Balance_Kp=300,Balance_Kd=1,Velocity_Kp=80,Velocity_Ki=0.4;//PID参数
u16 PID_Parameter[10],Flash_Parameter[10];  //Flash相关数组
float Zhongzhi=1;                          //机械中值
u32 Remoter_Ch1=1500,Remoter_Ch2=1500;      //航模遥控接收变量

/* ★ B（固件）2026-10-09 新增（决策 44）：转向环的两个增益改为**全局可调**，
   以便协议 0x24（PID 在线调参）能覆盖它们。
   ⚠️ 默认值与厂商 turn() 里原来的**局部常量完全一致**（Kp=52；前后运动时 Kd=0.5），
      所以在没有被调参之前，控制行为与基线**逐位相同**。
      对应关系：0x07 的"Turn_Kd"即基线里"有前后指令时"的 Kd（无前后指令时基线行为是 Kd=0）。*/
float Turn_Kp=52;                           //转向环 P（基线 turn() 内局部常量）
float Turn_Kd=0.5f;                         //转向环 D（基线：仅前后运动时取 0.5）

int main(void)
{
	delay_init(168);                //=====主频168M
#ifdef USE_FREERTOS
	uart_init(CFG_BAUD_WIRE);       //=====USART1 = **有线调试链路**（同协议，PID 调参用，决策 45）
#else
	uart_init(128000);              //=====USART1 = 厂商 DataScope 波形通道（裸机对照模式）
#endif
#ifdef USE_FREERTOS
	/* ★ 混合式架构（决策 37）：FreeRTOS 要求 4 位全部作抢占优先级。
	   控制中断 EXTI15_10 与串口中断的抢占优先级都是 0/1（数值 < configMAX_SYSCALL(5)），
	   因此它们**不会被调度器延迟**，但也**绝不能调用 RTOS API** ——
	   两个中断都遵守这一点（串口只往环形缓冲塞字节）。 */
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
#else
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);//=====设置系统中断优先级分组2
#endif
	LED_Init();                     //=====LED初始化
	KEY_Init();                     //=====按键初始化
  OLED_Init();                    //=====OLED初始化
	TIM3_Cap_Init(0XFFFF,84-1);	    //=====超声波初始化
	TIM8_Cap_Init(0XFFFF,168-1);	  //=====航模遥控接收初始
	Encoder_Init_TIM2();            //=====编码器初始化
	Encoder_Init_TIM4();            //=====编码器初始化
	uart2_init(115200);             //=====串口2（蓝牙）：协议要求 115200 8N1
	delay_ms(500);                  //=====延时等待系统稳定
	IIC_Init();                     //=====IIC初始化
  MPU6050_initialize();           //=====MPU6050初始化	
  DMP_Init();                     //=====初始化DMP 
	Adc_Init();                     //=====模拟量采集初始化（ADC1：电池）
	//NRF24L01_Init();    					//=====初始化NRF24L01  
  //while(NRF24L01_Check());      //=====NRF24L01模块自检，开启之后必须接入NRF24L01模块，程序才继续执行
	CAN1_Mode_Init(1,7,6,3,CAN_Mode_Normal);//=====CAN初始化  
	MiniBalance_PWM_Init(8400,1);   //=====PWM初始化
	MiniBalance_EXTI_Init();        //=====外部中断初始化（★ 5ms/10ms 控制节拍从这里开始）

#ifdef USE_FREERTOS
	/* ★ 此后控制中断已经在跑（姿态+三环 PID 时序与基线**完全一致**），
	   我们只是把"可以并发等待"的部分（采集/通信/状态机/界面）交给 FreeRTOS。
	   App_Start() 内部会建 4 个任务并启动调度器，**不返回**。 */
	App_Start();
	for(;;);                        //=====兜底（App_Start 正常情况下不返回）
#else
  while(1){
			if(Flash_Send==1)        //写入PID参数到Flash,由app控制该指令
				{
					Flash_Write();	
					Flash_Send=0;	
				}	
			if(Flag_Show==0)        	  //使用MiniBalance APP和OLED显示屏
				{
						APP_Show();	
						oled_show();          //===显示屏打开
				}
				else                      //使用MiniBalance上位机 上位机使用的时候需要严格的时序，故此时关闭app监控部分和OLED显示屏
				{
						DataScope();          //开启MiniBalance上位机
				}	
				delay_flag=1;	
				delay_50=0;
				while(delay_flag);	     //通过MPU6050的INT中断实现的50ms精准延时	
	}
#endif
}
