#ifndef __GAS_H
#define __GAS_H

/*
 * MQ-2 / MQ-135 气体传感通道（**纯算法层**，无任何 ST 头文件）
 *
 * 依据：
 *   · docs/research/MQ气体传感器开源实现.md（算法与参数逐行核实过来源）
 *   · docs/decision 记录 13~18（双通道 + 分压 + Rs/R0 为主链路 + ppm 作估计值）
 *
 * 链路：ADC 原始值 → 反分压得 AO 电压 → Rs → Rs/R0（相对量，主链路）
 *        → 温湿度补偿 → ppm 估计值（= a·ratio^b，datasheet 曲线拟合）
 *
 * ★ 电气前提（见 docs/handoff/交接说明.md §2.2）：
 *     MQ 的 AO 在 5V 供电下最高接近 5V，板上经 R 分压（10k 串联 + 15k 对地）降到 0.6 倍后进 MCU，
 *     所以 Vao = Vadc / 0.6 = Vadc × (Rtop+Rbottom)/Rbottom。
 *     而 STM32 的 ADC 参考是 3.3V，故 Vadc = raw × 3300mV / 4095。
 */

#include <stdint.h>

/* 滑窗长度（MQ 输出噪声不小，取 8 点滑动平均） */
#define GAS_AVG_WINDOW 8

typedef struct
{
	/* ---- 电气参数（到货后需实测/标定，见 README §五） ---- */
	uint16_t rlOhm;          /* 负载电阻 RL（Ω）——**必须实测**，默认 10000 */
	uint16_t vccMv;          /* 加热丝供电（mV），默认 5000 */
	uint16_t adcVrefMv;      /* ADC 参考（mV），默认 3300 */
	uint16_t divTopOhm;      /* 串联分压电阻（AO 侧），默认 10000 */
	uint16_t divBottomOhm;   /* 对地分压电阻，默认 15000 */

	/* ---- 传感器特性 ---- */
	float    ratioCleanAir;  /* 清洁空气中 Rs/R0：MQ-2 = 9.83，MQ-135 = 3.6 */
	float    ppmA;           /* ppm = a · ratio^b */
	float    ppmB;
} GasChannelCfg;

typedef struct
{
	GasChannelCfg cfg;

	float    r0;             /* 标定得到的 R0（Ω）；0 = 尚未标定 */
	float    r0Sum;          /* 标定累加（Rs 之和） */
	uint16_t r0Count;

	float    ratioBuf[GAS_AVG_WINDOW];  /* 滑动平均窗口（存补偿后的 ratio） */
	uint8_t  ratioIdx;
	uint8_t  ratioFilled;

	float    ratio;          /* 最近一次（补偿后有符号意义的）Rs/R0 */
	float    ratioAvg;       /* 滑动平均 */
	float    ppm;            /* 最近一次 ppm 估计值 */
	float    rs;             /* 最近一次 Rs（Ω） */
	uint16_t lastRaw;        /* 最近一次的 ADC 原始值（组包上报要用） */

	uint16_t overCount;      /* 连续超阈值次数 */
	uint8_t  alarm;          /* 1 = 已确认超阈值（带迟滞） */
} GasChannel;

/* ---- 配置默认值（按本项目实际电路） ---- */
void Gas_DefaultCfg_MQ2(GasChannelCfg *cfg);
void Gas_DefaultCfg_MQ135(GasChannelCfg *cfg);

void  Gas_InitChannel(GasChannel *ch, const GasChannelCfg *cfg);

/* ---- 单步换算（每一步都独立可测） ---- */
float Gas_AdcToVadcMv(const GasChannelCfg *cfg, uint16_t adcRaw);   /* ADC 引脚电压 */
float Gas_AdcToVaoMv(const GasChannelCfg *cfg, uint16_t adcRaw);    /* 反分压后的 MQ AO 电压 */
float Gas_VaoToRs(const GasChannelCfg *cfg, float vaoMv);           /* Rs = VCC·RL/Vao − RL */
/* 温湿度补偿系数（GeorgK 公式）。**参考点 ≈19.4 ℃ / 33 %RH 时为 1.0**，
   本工程按"除数"使用（ratio_corr = ratio / factor），原因见 gas.c 顶部注释。 */
float Gas_CorrectionFactor(float tempC, float humiRh);

/*
 * 主入口：喂一个 ADC 原始值，返回补偿 + 滑动平均后的 ratio。
 *   tempC/humiRh：DHT11 读数；compEnable=0 或 DHT 无效时传 25.0/50.0 且 compEnable=0
 *   ppm 结果写在 ch->ppm（未标定 R0 时为 0）
 */
float Gas_Update(GasChannel *ch, uint16_t adcRaw, float tempC, float humiRh, int compEnable);

/* ---- R0 标定：清洁空气中多次累加后取平均 ---- */
void  Gas_CalibrateAdd(GasChannel *ch, uint16_t adcRaw);
float Gas_CalibrateFinish(GasChannel *ch);   /* 返回算出的 R0，并写入 ch->r0 */

/* ---- 阈值研判：多帧确认 + 迟滞（释放门限 = 90% 阈值） ----
   返回 1 表示"本次确认超阈值"（上升沿），0 表示未超或已处于报警态 */
int   Gas_UpdateAlarm(GasChannel *ch, uint16_t threshPpmX10, uint8_t confirmFrames);

/* ---- ppm 换算（供上位机/显示复用） ---- */
float Gas_RatioToPpm(const GasChannelCfg *cfg, float ratio);

#endif /* __GAS_H */
