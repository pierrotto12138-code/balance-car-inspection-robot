#include "control.h"
#include "filter.h"
#include "MPU6050.h"
#include "inv_mpu.h"
#ifdef USE_FREERTOS
#include "pages.h"          /* USER 按键双击切 OLED 页（只改一个 volatile 变量，中断安全）*/
#endif
  /**************************************************************************
作者：平衡小车之家
我的淘宝小店：http://shop114407458.taobao.com/
**************************************************************************/
int Balance_Pwm,Velocity_Pwm,Turn_Pwm;
u8 Flag_Target;
u32 Flash_R_Count;
int Voltage_Temp,Voltage_Count,Voltage_All;

#ifdef USE_FREERTOS
/* ★ B（固件）2026-10-09（决策 47）：安全停车"真正落到电机"的那一环。
   任务层（TaskTask）把 `Patrol_MotorsDisabled()` 的结果写进这个变量，
   中断在这里读它并**强制** Flag_Stop=1（厂商的 Turn_Off() 已经会据此关电机）。
   为什么放在中断里做：中断每 5/10 ms 都会跑，是唯一能保证"锁存一置位、电机立刻停"的地方；
   而任务侧写一个 volatile u8 是原子的，不需要任何锁。 */
extern volatile u8 App_MotorDisable;
/* ★ 2026-10-13：本拍是否真的执行了 Set_Pwm（供 0x16 上报，铁律 29 的断言位）*/
volatile u8 g_pwmApplied = 0;
#include "odom.h"           /* 里程累计（纯加法，中断安全）*/
#endif
/**************************************************************************
函数功能：所有的控制代码都在这里面
         5ms定时中断由MPU6050的INT引脚触发
         严格保证采样和数据处理的时间同步
**************************************************************************/
int EXTI15_10_IRQHandler(void)
{
		if(INT==0)
	{
			EXTI->PR=1<<15;                                                      //清除中断标志位
			Flag_Target=!Flag_Target;
			if(delay_flag==1)
				{
					if(++delay_50==10)	 delay_50=0,delay_flag=0;                     //给主函数提供50ms的精准延时
				}
			if(Flag_Target==1)                                                  //5ms读取一次陀螺仪和加速度计的值，更高的采样频率可以改善卡尔曼滤波和互补滤波的效果
			{
			Get_Angle(Way_Angle);                                               //===更新姿态
			Get_MC6();                                                        	//===读取航模遥控器的数据
#ifndef USE_FREERTOS
			if(++Flash_R_Count<=250&&Angle_Balance>30)Flash_Read();             //=====读取Flash的PID参数
#else
			/* ★ B 2026-10-13 隔离（审计发现"会错"级缺陷）：
			   厂商这条"上电斜握 >30° 就从 Flash 读回 PID"的路径，地址是 `FLASH_SAVE_ADDR=0x08020000`，
			   而**我们的持久化改到了 `CFG_PARAM_FLASH_ADDR=0x08040000`**（见 param_store.c），
			   RTOS 下没有任何人往 0x08020000 写过 → 那里恒为 0xFFFFFFFF
			   → `stmflash.c` 的"空白即用默认值"判据恒真 → 把 Balance_Kp/Kd、Velocity_Kp/Ki
			   **静默写回 300/1/80/0.4**，正好覆盖 `ParamStore_Load()` 刚载入的调参
			   （现象："参数明明存了，重启后偶尔变回默认"，且没有任何上报）。
			   另外厂商 `Flash_Read()` 把 10 个 u32（40 B）读进 `u16 PID_Parameter[10]`（20 B），
			   本身就是越界写。
			   ⇒ RTOS 构建下**参数持久化的唯一权威是 ParamStore**，这条厂商路径必须整段隔离。*/
#endif
      Voltage_Temp=Get_battery_volt();		                                //=====读取电池电压
			Voltage_Count++;                                                    //=====平均值计数器
			Voltage_All+=Voltage_Temp;                                          //=====多次采样累积
			if(Voltage_Count==100) Voltage=Voltage_All/100,Voltage_All=0,Voltage_Count=0;//=====求平均值
			return 0;
			}                                                                   //10ms控制一次，为了保证M法测速的时间基准，首先读取编码器数据
			Encoder_Left=Read_Encoder(2);                                    		//===读取编码器的值
			Encoder_Right=Read_Encoder(4);
#if CFG_ENCODER_DIR_INVERT
			#if CFG_ENCODER_LEFT_INVERT
			Encoder_Left=-Encoder_Left;
#endif
#if CFG_ENCODER_RIGHT_INVERT
			Encoder_Right=-Encoder_Right;
#endif
#endif                                   		//===读取编码器的值
#ifdef USE_FREERTOS
			/* ★ B（固件）2026-10-09（决策 49）：把这一拍（10 ms）的编码器计数累进里程计，
				供巡检状态机判断"走到下一个巡检点"。
				—— 为什么放在这里：Encoder_Left/Right 是**每 10 ms 的增量**，任务 100 ms 才跑一次，
					在任务里采样会漏掉 9/10 的计数；而本函数只有整数加减，放在中断里没有负担。 */
			Odom_AddCounts(Encoder_Left, Encoder_Right);
#endif
			Get_Angle(Way_Angle);                                               //===更新姿态
			Read_Distane();                                                			//===获取超声波测量距离值
				g_pwmApplied = 0;                      /* ★ 每拍先清，只有真调了 Set_Pwm 才置 1 */
			Balance_Pwm =balance(Angle_Balance,Gyro_Balance);                   //===平衡PID控制
		Velocity_Pwm=velocity(Encoder_Left,Encoder_Right);                  //===速度环PID控制	 记住，速度反馈是正反馈，就是小车快的时候要慢下来就需要再跑快一点
			Turn_Pwm    =turn(Encoder_Left,Encoder_Right,Gyro_Turn);          	//===转向环PID控制
			Moto1=Balance_Pwm+Velocity_Pwm-Turn_Pwm;                            //===计算左轮电机最终PWM
			Moto2=Balance_Pwm+Velocity_Pwm+Turn_Pwm;                            //===计算右轮电机最终PWM
			Xianfu_Pwm();                                                       //===PWM限幅
			if(Pick_Up(Acceleration_Z,Angle_Balance,Encoder_Left,Encoder_Right))//===检查是否小车被那起
			Flag_Stop=1;	                                                      //===如果被拿起就关闭电机
#ifdef USE_FREERTOS
			/* ★ 安全停车锁存期间，"放下自动使能"这条厂商逻辑必须被挡住，
				否则把车拿起来再放下就会**绕过安全停车**（决策 41 要求锁存、不自动起步）。*/
			if(Put_Down(Angle_Balance,Encoder_Left,Encoder_Right) && App_MotorDisable==0)
			Flag_Stop=0;	                                                      //===如果被放下就启动电机
			/* ★ 锁存优先：本拍强制关电机（Turn_Off 会据此真正把 PWM 置 0）*/
			if(App_MotorDisable) Flag_Stop=1;
#else
			if(Put_Down(Angle_Balance,Encoder_Left,Encoder_Right))              //===检查是否小车被放下
			Flag_Stop=0;	                                                      //===如果被放下就启动电机
#endif
      if(Turn_Off(Angle_Balance,Voltage)==0)                              //===如果不存在异常
				/* ★ 2026-10-13：分侧翻向（见 app_config.h 的方向开关说明）。
			   实测两轮在前后倾时"原地对拧" ⇒ 左右两侧不一致，须**分侧**处置。*/
#if CFG_MOTOR_LEFT_INVERT
				Moto1 = -Moto1;
#endif
#if CFG_MOTOR_RIGHT_INVERT
				Moto2 = -Moto2;
#endif
				Set_Pwm(Moto1,Moto2);                                               //===赋值给PWM寄存器
			if(Bi_zhang==0)Led_Flash(100);                                      //===LED闪烁;常规模式 1s改变一次指示灯的状态
			else           Led_Flash(0);                                        //===LED闪烁;超声波模式 指示灯常亮
			Key();                                                              //===扫描按键状态 单击双击可以改变小车运行状态
	}
		return 0;
}

/**************************************************************************
函数功能：直立PD控制
入口参数：角度、角速度
返回  值：直立控制PWM
作    者：平衡小车之家
**************************************************************************/
int balance(float Angle,float Gyro)
{
   float Bias;
		int balance;
		Bias=Angle-Zhongzhi;                       //===求出平衡的角度中值 和机械相关
		balance=Balance_Kp*Bias+Gyro*Balance_Kd;   //===计算平衡控制的电机PWM  PD控制   kp是P系数 kd是D系数
		return balance;
}

/**************************************************************************
函数功能：速度PI控制 修改前进后退速度，请修Target_Velocity，比如，改成60就比较慢了
入口参数：左轮编码器、右轮编码器
返回  值：速度控制PWM
作    者：平衡小车之家
**************************************************************************/
int velocity(int encoder_left,int encoder_right)
{
     static float Velocity,Encoder_Least,Encoder,Movement;
		static float Encoder_Integral,Target_Velocity;

		/* ★ B 2026-10-13 修正（平衡审查报告的 P0，也是"只改一处"的结论）：
		   停机/保护态时，把速度环的**全部**状态在函数入口一次清干净，并直接返回 0。
		   为什么必须改（三件事叠在一起）：
		     ① **顺序错**：原来只在函数末尾 `if(Turn_Off(...)||Flag_Stop) Encoder_Integral=0;`
		        清零，而这一拍在它之前**已经用旧积分算过 Velocity** —— 也就是"先用了再清"。
		        （该拍不会真驱动电机，因为主循环里 `Turn_Off` 同样返回 1 会跳过 `Set_Pwm`，
		          所以这是"算了不该算的"，不是"输出了不该输出的"。）
		     ② **Encoder / Encoder_Least 从不清零**：停机期间人工挪过轮子后重新使能，
		        第一拍会按"停机前的滤波速度"出力 → 起步抽一下，现场极易被误判成机械问题。
		     ③ **量级不对称**：积分满幅 `K_i×10000 = 4000 PWM`，而平衡环 P 项 1° 才 300 PWM
		        —— 积分一旦朝错误方向饱和，平衡环的整个工作范围都扳不回来（这就是
		        "速度环一调大就必摔"的机理）。
		   返回 0 让"停机 = 速度环零出力"成为明确语义，而不是靠末尾清零兜底。
		   判据用与本拍输出**同一拍**的 Angle_Balance/Voltage，与主循环一致；
		   同时把 `Turn_Off()` 每拍的调用次数从 2 降到 1（原来函数末尾还会再调一次）。*/
		if(Turn_Off(Angle_Balance,Voltage)==1||Flag_Stop==1)
		{
			Encoder_Integral=0;
			Encoder=0;
			Encoder_Least=0;
			Movement=0;
			return 0;
		}

		//=============遥控前进后退部分=======================//
		if(Bi_zhang!=0&&Flag_sudu==1)  Target_Velocity=55;                 //如果进入避障模式,自动进入低速模式
    else 	                         Target_Velocity=110;
		if(1==Flag_Qian)    	Movement=-Target_Velocity/Flag_sudu;	         //===前进标志位置1
		else if(1==Flag_Hou)	Movement=Target_Velocity/Flag_sudu;         //===后退标志位置1
		else  Movement=0;
		if(Bi_zhang==1&&Flag_Left!=1&&Flag_Right!=1)        //进入避障模式
		{
			if(Distance<500)  Movement=Target_Velocity/Flag_sudu;
		}
		if(Bi_zhang==2&&Flag_Left!=1&&Flag_Right!=1)        //进入跟随模式
		{
			if(Distance>100&&Distance<300)  Movement=-Target_Velocity/Flag_sudu;
		}
   //=============速度PI控制器=======================//
		Encoder_Least =(encoder_left+encoder_right)-0;                      //===获取最新速度偏差==测量速度（左右编码器之和）-目标速度（此处为零）
		Encoder *= 0.8f;		                                                //===一阶低通滤波器
		Encoder += Encoder_Least*0.2f;	                                    //===一阶低通滤波器
		Encoder_Integral +=Encoder;                                       	//===积分出位移 积分时间：10ms
		Encoder_Integral=Encoder_Integral-Movement;                      	 	//===接收遥控器数据，控制前进后退
		if(Encoder_Integral>10000)  	Encoder_Integral=10000;             	//===积分限幅
		if(Encoder_Integral<-10000)	Encoder_Integral=-10000;             		//===积分限幅
		Velocity=Encoder*Velocity_Kp+Encoder_Integral*Velocity_Ki;        	//===速度控制

		if(Flag_Hover==1)Zhongzhi=-Encoder/10-Encoder_Integral/300;       //这些斜坡悬停使用的算法

		/* ★ 2026-10-13：此处的"末尾清零"已上移到函数入口（见 velocity() 开头的说明）——
		   放在这里等于"先用旧积分算完 Velocity 再清零"，顺序是错的。*/      //===电机关闭后清除积分
		return Velocity;
}

/**************************************************************************
函数功能：转向控制  修改转向速度，请修改Turn_Amplitude即可
入口参数：左轮编码器、右轮编码器、Z轴陀螺仪
返回  值：转向控制PWM
作    者：平衡小车之家
**************************************************************************/
int turn(int encoder_left,int encoder_right,float gyro)//转向控制
{
		static float Turn_Target,Turn,Encoder_temp,Turn_Convert=0.9f,Turn_Count;
		float Turn_Amplitude=100/Flag_sudu;
		/* ★ B（固件）2026-10-09（决策 44）：原来的局部常量 Kp=52 / Kd=0.5 改为全局变量
			Turn_Kp / Turn_Kd（定义在 main.c，默认值与原常量完全相同），
			这样协议 0x24 才能在线调这两个增益；未被调参时行为与基线逐位一致。 */
		extern float Turn_Kp, Turn_Kd;
		float Kp=Turn_Kp, Kd=0;
		//=============遥控左右旋转部分=======================//
		if(1==Flag_Left||1==Flag_Right)                      //这一部分主要是根据旋转前的速度调整速度的起始速度，增加小车的适应性
		{
			if(++Turn_Count==1)
			Encoder_temp=myabs(encoder_left+encoder_right);
			Turn_Convert=50/Encoder_temp;
			if(Turn_Convert<0.6f)Turn_Convert=0.6f;
			if(Turn_Convert>3)Turn_Convert=3;
		}
		else
		{
			Turn_Convert=0.9f;
			Turn_Count=0;
			Encoder_temp=0;
		}
		if(1==Flag_Left)	           Turn_Target-=Turn_Convert;
		else if(1==Flag_Right)	     Turn_Target+=Turn_Convert;
		else Turn_Target=0;

    if(Turn_Target>Turn_Amplitude)  Turn_Target=Turn_Amplitude;    //===转向速度限幅
		if(Turn_Target<-Turn_Amplitude) Turn_Target=-Turn_Amplitude;
		if(Flag_Qian==1||Flag_Hou==1)  Kd=Turn_Kd;
		else Kd=0;   //转向的时候取消陀螺仪的纠正 有点模糊PID的思想
		//=============转向PD控制器=======================//
		Turn=-Turn_Target*Kp-gyro*Kd;                 //===结合Z轴陀螺仪进行PD控制
		return Turn;
}

/**************************************************************************
函数功能：赋值给PWM寄存器
入口参数：左轮PWM、右轮PWM
返回  值：无
**************************************************************************/
void Set_Pwm(int moto1,int moto2)
{
	g_pwmApplied = 1;    /* ★ 真正写寄存器的地方：这里置位，0x16 用它判定"算出来有没有被执行" */
		if(moto1>0)			AIN2=0,			AIN1=1;
			else 	          AIN2=1,			AIN1=0;
			PWMA=myabs(moto1)*1.17;
			if(moto2>0)	BIN1=0,			BIN2=1;
			else        BIN1=1,			BIN2=0;
			PWMB=myabs(moto2)*1.17;
}

/**************************************************************************
函数功能：限制PWM赋值
入口参数：无
返回  值：无
**************************************************************************/
void Xianfu_Pwm(void)
{
		int Amplitude=8200;    //===PWM满幅是8400 限制在8200
//		if(Flag_Qian==1)  Moto1+=DIFFERENCE;  //DIFFERENCE是一个衡量平衡小车电机和机械安装差异的一个变量。直接作用于输出，让小车具有更好的一致性。
//	  if(Flag_Hou==1)   Moto2-=DIFFERENCE;
    if(Moto1<-Amplitude) Moto1=-Amplitude;
		if(Moto1>Amplitude)  Moto1=Amplitude;
		if(Moto2<-Amplitude) Moto2=-Amplitude;
		if(Moto2>Amplitude)  Moto2=Amplitude;

}
/**************************************************************************
函数功能：按键修改小车运行状态
入口参数：无
返回  值：无
**************************************************************************/
void Key(void)
{
	u8 tmp,tmp2;
	tmp=click_N_Double(50);
#ifdef USE_FREERTOS
	/* ★ B（固件）2026-10-09（决策 47）：RTOS 模式下按键语义
			· 单击 = 启停（**但安全停车锁存期间无效**：禁止用按键绕过安全锁存）
			· 双击 = **切换 OLED 显示页**（原本是切 Flag_Show，而 RTOS 模式不用 DataScope，
					那条串口已改成有线协议链路 —— 见 pages.h 的说明）
			· 长按 = 普通/避障（厂商原语义，不变）                                              */
	if(tmp==1 && App_MotorDisable==0)Flag_Stop=!Flag_Stop;//单击控制小车的启停
	if(tmp==2)Pages_Next();              //双击切换 OLED 显示页
#else
	if(tmp==1)Flag_Stop=!Flag_Stop;//单击控制小车的启停
	if(tmp==2)Flag_Show=!Flag_Show;//双击控制小车的显示状态
#endif
	tmp2=Long_Press();
  if(tmp2==1) Bi_zhang=!Bi_zhang;		//长按控制小车是否进入超声波模式
}

/**************************************************************************
函数功能：异常关闭电机
入口参数：倾角和电压
返回  值：1：异常  0：正常
**************************************************************************/
u8 Turn_Off(float angle, int voltage)
{
		u8 temp;
		/* ★ B 2026-10-13 修正（审计"会炸"级缺陷 A5，与平衡直接相关）：
		   原来这句是**瞬时**判据 `voltage < 1110`（11.10 V），而 `Voltage` 是
		   **每 100 拍（约 0.5 s）才更新一次**的平均值（见本文件上面的采样累加）：
		     · 一旦某次均值落到 1110 以下 → 接下来 0.5 s 内该值**不会变** →
		       `Turn_Off()` 连续约 100 拍返回 1 → `Set_Pwm()` 整段时间不被调用、
		       方向脚被清零 → **电机 0.5 s 完全没有力矩**；
		     · 电压回升后立刻恢复 → 平衡车表现为"突然软脚趴下、又突然恢复"的**周期性抽搐**
		       （这正是"能平衡但会莫名摔倒"的典型成因）。
		   改成**迟滞 + 连续确认**，与 App 层低压判据（app_config.h 的 CFG_BAT_*）同一套思路：
		     · 低于 11.10 V 要**连续 50 拍**（0.25 s）才认定为低压并锁存；
		     · 回升到 **11.60 V 以上**才解除锁存（0.5 V 迟滞，避免在阈值上反复开关）。
		   为什么把状态机放在这里而不是 App 层：`Turn_Off()` 是**中断每拍**调用的执行端，
		   低压保护必须在这里生效才真正关得住电机（App 层的 `App_MotorDisable` 是另一条独立通路）。
		   真欠压时行为不变：照样停 —— 只是不再因"单次均值抖动"而抽风。*/
		{
			static u16 s_lowBatCnt = 0;
			static u8  s_lowBatLatched = 0;

			if (voltage > 0 && voltage < 1110)
			{
				if (s_lowBatCnt < 50)
				{
					s_lowBatCnt++;
				}
				if (s_lowBatCnt >= 50)
				{
					s_lowBatLatched = 1;
				}
			}
			else if (voltage >= 1160)
			{
				s_lowBatLatched = 0;      /* 回升到 11.60 V 以上才解除（迟滞）*/
				s_lowBatCnt = 0;
			}

			if(angle<(-40+Zhongzhi)||angle>(40+Zhongzhi)||1==Flag_Stop||s_lowBatLatched)//倾角超限/已停车/低压锁存 → 关电机
			{	                                                 //===倾角大于40度关闭电机
      temp=1;                                            //===Flag_Stop置1关闭电机
			AIN1=0;
			AIN2=0;
			BIN1=0;
			BIN2=0;
      }
			else
      temp=0;
      return temp;
		}
}

/**************************************************************************
函数功能：获取角度 三种算法经过我们的调校，都非常理想
入口参数：获取角度的算法 1：DMP  2：卡尔曼 3：互补滤波
返回  值：无
**************************************************************************/
void Get_Angle(u8 way)
{
		float Accel_Y,Accel_Angle,Accel_Z,Gyro_X,Gyro_Z;
			Temperature=Read_Temperature();      //===读取MPU6050内置温度传感器数据，近似表示主板温度。
		if(way==1)                           //===DMP的读取在数据采集中断读取，严格遵循时序要求
			{
					Read_DMP();                      //===读取加速度、角速度、倾角
					Angle_Balance=-Roll;             //===更新平衡倾角
					Gyro_Balance=-gyro[0];            //===更新平衡角速度
					Gyro_Turn=gyro[2];               //===更新转向角速度
					Acceleration_Z=accel[2];         //===更新Z轴加速度计
			}
      else
      {
			Gyro_X=(I2C_ReadOneByte(devAddr,MPU6050_RA_GYRO_XOUT_H)<<8)+I2C_ReadOneByte(devAddr,MPU6050_RA_GYRO_XOUT_L);    //读取Y轴陀螺仪
			Gyro_Z=(I2C_ReadOneByte(devAddr,MPU6050_RA_GYRO_ZOUT_H)<<8)+I2C_ReadOneByte(devAddr,MPU6050_RA_GYRO_ZOUT_L);    //读取Z轴陀螺仪
			Accel_Y=(I2C_ReadOneByte(devAddr,MPU6050_RA_ACCEL_YOUT_H)<<8)+I2C_ReadOneByte(devAddr,MPU6050_RA_ACCEL_YOUT_L); //读取X轴加速度计
			Accel_Z=(I2C_ReadOneByte(devAddr,MPU6050_RA_ACCEL_ZOUT_H)<<8)+I2C_ReadOneByte(devAddr,MPU6050_RA_ACCEL_ZOUT_L); //读取Z轴加速度计
			if(Gyro_X>32768)  Gyro_X-=65536;                       //数据类型转换  也可通过short强制类型转换
			if(Gyro_Z>32768)  Gyro_Z-=65536;                       //数据类型转换
			if(Accel_Y>32768) Accel_Y-=65536;                      //数据类型转换
			if(Accel_Z>32768) Accel_Z-=65536;                      //数据类型转换
			Gyro_Balance=Gyro_X;                                  //更新平衡角速度
			Accel_Angle=atan2(Accel_Y,Accel_Z)*180/PI;                 //计算倾角
			Gyro_X=Gyro_X/16.4f;                                    //陀螺仪量程转换
      if(Way_Angle==2)		  	Kalman_Filter(Accel_Angle,Gyro_X);//卡尔曼滤波
			else if(Way_Angle==3)   Yijielvbo(Accel_Angle,Gyro_X);    //互补滤波
		Angle_Balance=angle;                                   //更新平衡倾角
			Gyro_Turn=Gyro_Z;                                      //更新转向角速度
			Acceleration_Z=Accel_Z;                                //===更新Z轴加速度计
		}
}
/**************************************************************************
函数功能：绝对值函数
入口参数：int
返回  值：unsigned int
**************************************************************************/
int myabs(int a)
{
		int temp;
		if(a<0)  temp=-a;
		else temp=a;
		return temp;
}
/**************************************************************************
函数功能：检测小车是否被拿起
入口参数：int
返回  值：unsigned int
**************************************************************************/
int Pick_Up(float Acceleration,float Angle,int encoder_left,int encoder_right)
{
		static u16 flag,count0,count1,count2;
	if(flag==0)                                                                   //第一步
		{
			if(myabs(encoder_left)+myabs(encoder_right)<30)                         //条件1，小车接近静止
				count0++;
        else
        count0=0;
        if(count0>10)
			flag=1,count0=0;
		}
		if(flag==1)                                                                  //进入第二步
		{
			if(++count1>200)       count1=0,flag=0;                                 //超时不再等待2000ms
			if(Acceleration>26000&&(Angle>(-20+Zhongzhi))&&(Angle<(20+Zhongzhi)))   //条件2，小车是在0度附近被拿起
			flag=2;
		}
		if(flag==2)                                                                  //第三步
		{
			if(++count2>100)       count2=0,flag=0;                                   //超时不再等待1000ms
		if(myabs(encoder_left+encoder_right)>160)                                 //条件3，小车的轮胎因为正反馈达到最大的转速
      {
				flag=0;
				return 1;                                                               //检测到小车被拿起
			}
		}
	return 0;
}
/**************************************************************************
函数功能：检测小车是否被放下
入口参数：int
返回  值：unsigned int
**************************************************************************/
int Put_Down(float Angle,int encoder_left,int encoder_right)
{
		static u16 flag,count;
		if(Flag_Stop==0)                           //防止误检
   return 0;
		if(flag==0)
		{
			if(Angle>(-10+Zhongzhi)&&Angle<(10+Zhongzhi)&&encoder_left==0&&encoder_right==0)         //条件1，小车是在0度附近的
			flag=1;
		}
		if(flag==1)
		{
			if(++count>50)                                          //超时不再等待 500ms
			{
				count=0;flag=0;
			}
		if(encoder_left<-3&&encoder_right<-3&&encoder_left>-60&&encoder_right>-60)                //条件2，小车的轮胎在未上电的时候被人为转动
      {
				flag=0;
				flag=0;
				return 1;                                             //检测到小车被放下
			}
		}
	return 0;
}


/**************************************************************************
函数功能：采集遥控器的信号
入口参数：无
返回  值：无
**************************************************************************/
void  Get_MC6(void)
{
	if(Flag_Left==0&&Flag_Right==0)
	{
			if((Remoter_Ch1>1650&&Remoter_Ch1<2100)||(Remoter_Ch1>21650&&Remoter_Ch1<22100))	Flag_Qian=1,Flag_Hou=0,Flag_sudu=1;//////////////前
	else if((Remoter_Ch1<1350&&Remoter_Ch1>900) ||(Remoter_Ch1<21350&&Remoter_Ch1>20900))	Flag_Qian=0,Flag_Hou=1,Flag_sudu=1;//////////////后
  else if ((Remoter_Ch1>1350&&Remoter_Ch1<1650) ||(Remoter_Ch1>21350&&Remoter_Ch1<21650))	Flag_Qian=0,Flag_Hou=0;//////////////停
	}
	if(Flag_Qian==0&&Flag_Hou==0)
	{
				if((Remoter_Ch2>1650&&Remoter_Ch2<2100)||(Remoter_Ch2>21650&&Remoter_Ch2<22100))Flag_Left=1,Flag_Right=0,Flag_sudu=1;//////////////左
		else if((Remoter_Ch2<1350&&Remoter_Ch2>900) ||(Remoter_Ch2<21350&&Remoter_Ch2>20900))Flag_Left=0,Flag_Right=1,Flag_sudu=1;//////////////右
		else if ((Remoter_Ch2>1350&&Remoter_Ch2<1650) ||(Remoter_Ch2>21350&&Remoter_Ch2<21650))Flag_Left=0,Flag_Right=0;//////////////停
	}
}
