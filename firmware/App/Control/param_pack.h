#ifndef __PARAM_PACK_H
#define __PARAM_PACK_H

/*
 * PID 参数的**打包/解包**（纯逻辑，可在 PC 上测；不碰 Flash）
 *
 * 为什么单独拆出来：Flash 读写要 STM32 寄存器，无法离线测；
 * 但"存进去的和读出来的必须一模一样"这件事**必须**能离线验证，
 * 否则会出现"调好的参数存了、重启却回到默认值"这种极难查的问题。
 *
 * 存储格式（每个参数 1 个 32 位字，**直接存 float 位型**，不做定点缩放，避免精度损失）：
 *     word[0] = MAGIC
 *     word[1] = 参数个数
 *     word[2..] = 各参数 float 位型（顺序见 ParamId）
 */

#include <stdint.h>

/* 参数 ID（与协议 0x24 一致，见 docs/protocol/communication_protocol.md §4.2） */
#define PARAM_BALANCE_KP   0x01u
#define PARAM_BALANCE_KD   0x02u
#define PARAM_VELOCITY_KP  0x03u
#define PARAM_VELOCITY_KI  0x04u
#define PARAM_ZHONGZHI     0x05u
#define PARAM_TURN_KP      0x06u
#define PARAM_TURN_KD      0x07u

/* 0x24 的特殊参数 ID（不落在上面 7 个参数里，见协议 §4.2 扩展） */
#define PARAM_ID_DUMP_ALL  0xFFu   /* 请求回读全部参数（回 7 条 0x24）*/
#define PARAM_ID_SAVE      0x00u   /* 把当前参数存入 Flash */
#define PARAM_ID_LOAD      0xFEu   /* 从 Flash 重新载入 */
#define PARAM_ID_TUNE_MODE 0xFD    /* 调参观测模式开关（value: 0/1）—— 100 Hz 上报 */

#define PARAM_COUNT        7u
#define PARAM_WORDS        (2u + PARAM_COUNT)   /* magic + count + 7 个参数 */

/* 可调参数的当前值（内存镜像，与厂商全局量同步） */
typedef struct
{
	float balanceKp;
	float balanceKd;
	float velocityKp;
	float velocityKi;
	float zhongzhi;
	float turnKp;
	float turnKd;
} ParamSet;

/* 顺序必须与 PARAM_COUNT/PARAM_WORDS 的组织方式一致 */
void   ParamPack_Default(ParamSet *p);

/* ★ 本层**不碰厂商全局量**（保证可在 PC 上编译测试）；
   "参数表 ↔ 厂商全局量"的同步在 param_store.c 里（那是 STM32 层）。*/
void   ParamPack_FromGlobals(ParamSet *p);
void   ParamPack_ToGlobals(const ParamSet *p);

/* 按参数 ID 取值/设值；返回 0=成功，-1=ID 无效 */
int    ParamPack_Get(const ParamSet *p, uint8_t id, float *value);
int    ParamPack_Set(ParamSet *p, uint8_t id, float value);

/* 打包到 word 数组（用于写 Flash）；返回写入的字数 */
uint16_t ParamPack_Pack(const ParamSet *p, uint32_t magic, uint32_t *words, uint16_t capWords);
/* 从 word 数组解包；magic 不符 / 个数不符 / 数值离谱 → 返回 -1（调用方应退回默认值） */
int      ParamPack_Unpack(const uint32_t *words, uint16_t capWords,
                          uint32_t magic, ParamSet *out);

#endif /* __PARAM_PACK_H */
