#ifndef BSP_OLED_OLED_H
#define BSP_OLED_OLED_H
#include "sys.h"
  /**************************************************************************
作者：平衡小车之家
我的淘宝小店：http://shop114407458.taobao.com/
**************************************************************************/
//-----------------OLED端口定义----------------
/* ★ B 2026-10-13：累计写入字节数（0x15 诊断帧上报）。
   用途：区分"屏幕根本没被写"（计数不涨）与"写了但显示不对"（计数在涨）——这两类故障的排查方向完全不同。*/
extern volatile uint32_t g_oledWrites;
#define OLED_RST_Clr() PCout(15)=0   //RST
#define OLED_RST_Set() PCout(15)=1   //RST

#define OLED_RS_Clr() PCout(0)=0    //DC
#define OLED_RS_Set() PCout(0)=1    //DC

#define OLED_SCLK_Clr()  PCout(13)=0  //SCL
#define OLED_SCLK_Set()  PCout(13)=1   //SCL

#define OLED_SDIN_Clr()  PCout(14)=0   //SDA
#define OLED_SDIN_Set()  PCout(14)=1   //SDA

#define OLED_CMD  0	//写命令
#define OLED_DATA 1	//写数据
//OLED控制用函数
void OLED_WR_Byte(u8 dat,u8 cmd);
void OLED_Display_On(void);
void OLED_Display_Off(void);
void OLED_Refresh_Gram(void);
void OLED_Init(void);
void OLED_Clear(void);
void OLED_DrawPoint(u8 x,u8 y,u8 t);
void OLED_ShowChar(u8 x,u8 y,u8 chr,u8 size,u8 mode);
void OLED_ShowNumber(u8 x,u8 y,u32 num,u8 len,u8 size);
/* ★ B 2026-10-13：参数字符串类型由 `const u8 *` 改为 `const char *`。
   原因：厂商所有调用点都直接传字符串字面量（`"Kalman"` 等），在 GCC 下会产生几十条
   `-Wpointer-sign`（`char*` 传给 `u8*`）。改成 `char *` 与调用点语义一致，
   内部仍按字节查字库（见 oled.c 的实现），**不改变任何运行时行为**。*/
void OLED_ShowString(u8 x,u8 y,const char *p);
#endif
