#ifndef __BUZZER_H
#define __BUZZER_H

/*
 * 蜂鸣器（底板 J18，**有源模块 + 低电平触发**，接 PC5 —— 决策 26）
 *
 * 注意与厂商基线的区别：
 *   基线用的是 `BZ1 + Q1` 三极管驱动（高电平响），新底板改成"有源模块直接驱动、低电平响"，
 *   所以极性是**反的**：置低 = 响。
 *
 * 本层是硬件层（不参与 PC 原生测试）；报警策略（谁在什么时候响）由 UiTask 决定。
 */

#include <stdint.h>

void Buzzer_Init(void);
void Buzzer_On(void);        /* 响（PC5 = 0）*/
void Buzzer_Off(void);       /* 停（PC5 = 1）*/
void Buzzer_Set(int on);
int  Buzzer_IsOn(void);

#endif /* __BUZZER_H */
