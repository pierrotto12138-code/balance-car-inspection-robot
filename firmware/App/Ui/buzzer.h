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
 *
 * ★ 2026-10-13 复查清理：删掉了 `Buzzer_IsOn()` 与它背后的 `s_on` 影子状态 ——
 *   没有任何调用方，而且"蜂鸣器现在响不响"的唯一真相就是 **PC5 引脚电平**，
 *   再维护一份软件副本只会形成第二个真相（见编码规范 §二）。
 */

#include <stdint.h>

void Buzzer_Init(void);
void Buzzer_On(void);        /* 响（PC5 = 0）*/
void Buzzer_Off(void);       /* 停（PC5 = 1）*/
void Buzzer_Set(int on);

#endif /* __BUZZER_H */
