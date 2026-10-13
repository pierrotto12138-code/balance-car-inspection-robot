#ifndef APP_CONTROL_PARAM_STORE_H
#define APP_CONTROL_PARAM_STORE_H

/*
 * PID 参数的 Flash 持久化（STM32 层；打包逻辑在 param_pack.c，可离线测）
 *
 * 地址：`CFG_PARAM_FLASH_ADDR = 0x08040000`（扇区 6 起始，128 KB 一扇区）
 *   · 厂商原本用 0x08020000（扇区 5）存它自己的 4 个参数；我们**不复用、不覆盖**它；
 *   · 本工程代码约 60 KB（< 0x08010000），离扇区 6 很远，不会自撞；
 *   · 注意：`STMFLASH_Write()` 是**整扇区擦除**后再写，所以本函数只在"保存参数"时调用，
 *     不要放进周期任务。
 *
 * 用法：
 *   上电：ParamStore_LoadOrDefault()  —— 有有效参数就用，没有就保持默认值
 *   调参：ParamStore_Save()           —— 由 0x24 paramId=0x00 触发
 */

#include "param_pack.h"

/* 从上电默认值初始化（不做 Flash 访问）*/
void ParamStore_InitDefaults(void);

/* 从 Flash 载入；成功返回 0，未写过/无效返回 -1（此时保持当前值 = 默认值）*/
int  ParamStore_Load(void);

/* 把当前参数写入 Flash；成功返回 0 */
int  ParamStore_Save(void);

/* 由参数 ID 写一个值（并同步到厂商全局量）；返回 0=成功 */
int  ParamStore_SetOne(uint8_t id, float value);

/* 由参数 ID 读一个值；返回 0=成功 */
int  ParamStore_GetOne(uint8_t id, float *value);

#endif /* APP_CONTROL_PARAM_STORE_H */
