#ifndef APP_CONTROL_CONTROL_H
#define APP_CONTROL_CONTROL_H
#include "sys.h"
  /**************************************************************************
作者：平衡小车之家
我的淘宝小店：http://shop114407458.taobao.com/
**************************************************************************/
#define PI 3.14159265

#define DIFFERENCE 100
extern	int Balance_Pwm,Velocity_Pwm,Turn_Pwm;
/* ★ 2026-10-15：左右倾角/角速度（0x10 的 roll / gy；定义在 control.c）。
   `Angle_LR` 是**仅加速度计的静态倾角**（未做陀螺仪融合）—— 界面/文档要写明"静态"。*/
extern	float Angle_LR, Gyro_LR;
int EXTI15_10_IRQHandler(void);
int balance(float angle,float gyro);
int velocity(int encoder_left,int encoder_right);
int turn(int encoder_left,int encoder_right,float gyro);
void Set_Pwm(int moto1,int moto2);
void Key(void);
void Xianfu_Pwm(void);
u8 Turn_Off(float angle, int voltage);
void Get_Angle(u8 way);
int myabs(int a);
int Pick_Up(float Acceleration,float Angle,int encoder_left,int encoder_right);
int Put_Down(float Angle,int encoder_left,int encoder_right);
void  Get_MC6(void);
#endif
