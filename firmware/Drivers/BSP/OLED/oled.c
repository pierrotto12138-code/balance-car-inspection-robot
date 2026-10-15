#include "oled.h"
#include "stdlib.h"
#include "oledfont.h"
#include "delay.h"
  /**************************************************************************
作者：平衡小车之家
我的淘宝小店：http://shop114407458.taobao.com/
**************************************************************************/
u8 OLED_GRAM[128][8];
/* ★ B 2026-10-13：OLED 累计写入字节数（0x15 诊断帧上报，见 OLED_WR_Byte 处的说明）*/
volatile uint32_t g_oledWrites = 0;

/* ★ B 2026-10-13：软 SPI 位间空循环次数（≈每次 100 ns 量级）。
   取值理由见 OLED_WR_Byte 里的长注释：目的是把 SCL 压到 SSD1306 能接受的范围（<≈10 MHz）。
   现场若仍有乱码，先试加大到 16 / 32；若显示正常但刷新偏慢，可减小。*/
#ifndef OLED_BIT_DELAY_LOOPS
#define OLED_BIT_DELAY_LOOPS 8
#endif
void OLED_Refresh_Gram(void)
{
	u8 i,n;
	for(i=0;i<8;i++)
	{
		OLED_WR_Byte (0xb0+i,OLED_CMD);    //设置页地址（0~7）
		OLED_WR_Byte (0x00,OLED_CMD);      //设置显示位置—列低地址
		OLED_WR_Byte (0x10,OLED_CMD);      //设置显示位置—列高地址
		for(n=0;n<128;n++)OLED_WR_Byte(OLED_GRAM[n][i],OLED_DATA);
	}
}

//向OLED写入一个字节。
//dat:要写入的数据/命令
//cmd:数据/命令标志 0,表示命令;1,表示数据;
void OLED_WR_Byte(u8 dat,u8 cmd)
{
	u8 i;
	/* ★ B 2026-10-13：累计写入计数，供 0x15 诊断帧上报。
	   为什么值得加：OLED 是现场唯一的可视化输出，而"屏幕有没有在被刷"在协议里本来
	   完全看不到 —— 联调时只能靠人眼看屏。有了这个计数，工具端按秒差分即可客观区分
	   "根本没画"（计数不涨）与"画了但屏上不对"（计数在涨）——这两种故障的排查方向完全不同。*/
	g_oledWrites++;
	if(cmd)
		OLED_RS_Set();
	else
		OLED_RS_Clr();
	for(i=0;i<8;i++)
	{
		OLED_SCLK_Clr();
		if(dat&0x80)
			OLED_SDIN_Set();
		else
			OLED_SDIN_Clr();
		OLED_SCLK_Set();
		dat<<=1;
		/* ★ B 2026-10-13：位间插入短延时（可调），把 SCL 频率压到模块能接受的范围。
		   为什么必须加：本驱动是**纯 GPIO 位带硬翻转、一行延时都没有**（见下面 8 次循环）。
		   同一份源码用不同编译器编出来的实际翻转速度差别很大：
		     · 厂商用 Keil/ARMCC → 每条位带写要好几条指令，SCL 大致在模块可接受的量级；
		     · 我们用 GCC -O2 → 位带写被优化得很短，SCL 可能到 20 MHz 量级，
		       而 SSD1306 的 SCL 上限约 10 MHz，**超了就会丢命令** ——
		       典型现象正是"页地址命令没生效，所有文字全挤进第 0 页"（=只看到第一行）。
		   这里用 N 次空循环凑出 ~100 ns 级延时：既显著降低 SCL，又不至于拖慢刷新
		   （1 KB 全屏刷新 ≈ 8 ms，10 Hz 刷新下占用约 8% CPU，可以接受）。
		   若现场仍有乱码：把 OLED_BIT_DELAY_LOOPS 加大（16/32），或反过来减小。*/
		{
			volatile u8 dly = OLED_BIT_DELAY_LOOPS;
			while (dly-- != 0)
			{
			}
		}
	}
	OLED_RS_Set();
}


//开启OLED显示
void OLED_Display_On(void)
{
	OLED_WR_Byte(0X8D,OLED_CMD);  //SET DCDC命令
	OLED_WR_Byte(0X14,OLED_CMD);  //DCDC ON
	OLED_WR_Byte(0XAF,OLED_CMD);  //DISPLAY ON
}
//关闭OLED显示
void OLED_Display_Off(void)
{
	OLED_WR_Byte(0X8D,OLED_CMD);  //SET DCDC命令
	OLED_WR_Byte(0X10,OLED_CMD);  //DCDC OFF
	OLED_WR_Byte(0XAE,OLED_CMD);  //DISPLAY OFF
}
//清屏函数,清完屏,整个屏幕是黑色的!和没点亮一样!!!
void OLED_Clear(void)
{
	u8 i,n;
	for(i=0;i<8;i++)for(n=0;n<128;n++)OLED_GRAM[n][i]=0X00;
	OLED_Refresh_Gram();//更新显示
}
//画点
//x:0~127
//y:0~63
//t:1 填充 0,清空
void OLED_DrawPoint(u8 x,u8 y,u8 t)
{
	u8 pos,bx,temp=0;
	if(x>127||y>63)return;//超出范围了.
	pos=7-y/8;
	bx=y%8;
	temp=1<<(7-bx);
	if(t)OLED_GRAM[x][pos]|=temp;
	else OLED_GRAM[x][pos]&=~temp;
}

//在指定位置显示一个字符,包括部分字符
//x:0~127
//y:0~63
//mode:0,反白显示;1,正常显示
//size:选择字体 16/12
void OLED_ShowChar(u8 x,u8 y,u8 chr,u8 size,u8 mode)
{
	u8 temp,t,t1;
	u8 y0=y;
	chr=chr-' ';//得到偏移后的值
    for(t=0;t<size;t++)
    {
		if(size==12)temp=oled_asc2_1206[chr][t];  //调用1206字体
		else temp=oled_asc2_1608[chr][t];		 //调用1608字体
        for(t1=0;t1<8;t1++)
		{
			if(temp&0x80)OLED_DrawPoint(x,y,mode);
			else OLED_DrawPoint(x,y,!mode);
			temp<<=1;
			y++;
			if((y-y0)==size)
			{
				y=y0;
				x++;
				break;
			}
		}
    }
}
//m^n函数
u32 oled_pow(u8 m,u8 n)
{
	u32 result=1;
	while(n--)result*=m;
	return result;
}
//显示2个数字
//x,y :起点坐标
//len :数字的位数
//size:字体大小
//mode:模式	0,填充模式;1,叠加模式
//num:数值(0~4294967295);
void OLED_ShowNumber(u8 x,u8 y,u32 num,u8 len,u8 size)
{
	u8 t,temp;
	u8 enshow=0;
	for(t=0;t<len;t++)
	{
		temp=(num/oled_pow(10,len-t-1))%10;
		if(enshow==0&&t<(len-1))
		{
			if(temp==0)
			{
				OLED_ShowChar(x+(size/2)*t,y,' ',size,1);
				continue;
			}else enshow=1;

		}
			OLED_ShowChar(x+(size/2)*t,y,temp+'0',size,1);
	}
}
//显示字符串
//x,y:起点坐标
//*p:字符串起始地址
//用16字体
void OLED_ShowString(u8 x,u8 y,const char *p)
{
#define MAX_CHAR_POSX 122
#define MAX_CHAR_POSY 58
    while(*p!='\0')
    {
        if(x>MAX_CHAR_POSX){x=0;y+=16;}
        if(y>MAX_CHAR_POSY){y=x=0;OLED_Clear();}
        OLED_ShowChar(x,y,*p,12,1);
        x+=8;
        p++;
    }
}
//初始化OLED
void OLED_Init(void)
{
	GPIO_InitTypeDef  GPIO_InitStructure;
  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC  , ENABLE);  //使能GPIOC
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR  , ENABLE);    //PWR使能
	PWR_BackupAccessCmd(ENABLE);         //允许修改RTC 和后备寄存器
	RCC_LSEConfig(RCC_LSE_OFF);       //关闭外部低速外部时钟信号功能 后，PC13 PC14 PC15 才可以当普通IO用。
	GPIO_InitStructure.GPIO_Pin =  GPIO_Pin_0 |GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;  //PC0 PC13 PC14 PC15
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;   //输出
	GPIO_InitStructure.GPIO_OType =GPIO_OType_PP;
	GPIO_InitStructure.GPIO_Speed =GPIO_Speed_2MHz;  //2M
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;//根据设定参数初始化GPIO
	GPIO_Init(GPIOC, &GPIO_InitStructure);
	GPIOC->BSRRH = GPIO_Pin_0;
	GPIOC->BSRRH = GPIO_Pin_13;
	GPIOC->BSRRH = GPIO_Pin_14;
	GPIOC->BSRRH = GPIO_Pin_15;

	OLED_RST_Clr();
	delay_ms(100);
	OLED_RST_Set();

	OLED_WR_Byte(0xAE,OLED_CMD); //关闭显示
	OLED_WR_Byte(0xD5,OLED_CMD); //设置时钟分频因子,震荡频率
	OLED_WR_Byte(80,OLED_CMD);   //[3:0],分频因子;[7:4],震荡频率
	OLED_WR_Byte(0xA8,OLED_CMD); //设置驱动路数
	OLED_WR_Byte(0X3F,OLED_CMD); //默认0X3F(1/64)
	OLED_WR_Byte(0xD3,OLED_CMD); //设置显示偏移
	OLED_WR_Byte(0X00,OLED_CMD); //默认为0

	OLED_WR_Byte(0x40,OLED_CMD); //设置显示开始行 [5:0],行数.

	OLED_WR_Byte(0x8D,OLED_CMD); //电荷泵设置
	OLED_WR_Byte(0x14,OLED_CMD); //bit2，开启/关闭
	OLED_WR_Byte(0x20,OLED_CMD); //设置内存地址模式
	OLED_WR_Byte(0x02,OLED_CMD); //[1:0],00，列地址模式;01，行地址模式;10,页地址模式;默认10;
	OLED_WR_Byte(0xA1,OLED_CMD); //段重定义设置,bit0:0,0->0;1,0->127;
	OLED_WR_Byte(0xC0,OLED_CMD); //设置COM扫描方向;bit3:0,普通模式;1,重定义模式 COM[N-1]->COM0;N:驱动路数
	OLED_WR_Byte(0xDA,OLED_CMD); //设置COM硬件引脚配置
	OLED_WR_Byte(0x12,OLED_CMD); //[5:4]配置

	OLED_WR_Byte(0x81,OLED_CMD); //对比度设置
	OLED_WR_Byte(0xEF,OLED_CMD); //1~255;默认0X7F (亮度设置,越大越亮)
	OLED_WR_Byte(0xD9,OLED_CMD); //设置预充电周期
	OLED_WR_Byte(0xf1,OLED_CMD); //[3:0],PHASE 1;[7:4],PHASE 2;
	OLED_WR_Byte(0xDB,OLED_CMD); //设置VCOMH 电压倍率
	OLED_WR_Byte(0x30,OLED_CMD); //[6:4] 000,0.65*vcc;001,0.77*vcc;011,0.83*vcc;

	OLED_WR_Byte(0xA4,OLED_CMD); //全局显示开启;bit0:1,开启;0,关闭;(白屏/黑屏)
	OLED_WR_Byte(0xA6,OLED_CMD); //设置显示方式;bit0:1,反相显示;0,正常显示
	OLED_WR_Byte(0xAF,OLED_CMD); //开启显示
	OLED_Clear();
}
