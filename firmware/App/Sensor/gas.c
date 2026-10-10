#include "gas.h"
#include <math.h>

/*
 * MQ 通道算法 —— 与 tools/fw_prep/gen_sensor_vectors.py 里的 **Python 独立实现**逐项对齐
 * （由 firmware/tests/sensor_test.c 读 CSV 逐条比对，见 tools/fw_prep/run_sensor_test.py）。
 *
 * ── 关于温湿度补偿：**为什么用"除法"而不是"叠加"** ─────────────────────────
 * 开源里有两套用法，很容易混（docs/research/MQ气体传感器开源实现.md §4.2 专门警告过）：
 *     · MQUnifiedsensor： _ratio += correctionFactor      ← 叠加式
 *     · GeorgK/MQ135   ： Rs / correctionFactor           ← 除法式
 * 两种写法用的**是同一个公式**：
 *     factor = 0.00035·t² − 0.02718·t + 1.39538 − (h−33)·0.0018
 * 在参考条件（≈19.4 ℃ / 33 %RH）下 factor ≈ **1.0**。
 *   → 若按"叠加"用，相当于给 ratio 白加一个 1.0（MQ-135 清洁空气才 3.6，等于凭空 +28%），
 *     物理上讲不通；
 *   → 按"除法"用，参考条件下比值不变，偏离参考条件时才修正，这才符合"修正系数"的含义。
 * 所以本工程采用**除法**，并把 factor 单独暴露成 Gas_CorrectionFactor() 以便测试与复核。
 * ⚠️ 该系数本身来源不严谨（GeorgK 源码里自己写着 TODO 存疑），因此：
 *     · 默认**关闭**（由 0x23 的"温湿度补偿使能"控制，决策 18 也说明 ppm 只作估计值）；
 *     · 打开时必须按实测复核，报告里要注明来源与不确定性。
 *
 * ── 关于 ppm：**只作估计值**（决策 18） ─────────────────────────────────────
 * 主链路是 Rs/R0 相对量（跨通道比较、趋势、超阈值判定），ppm 仅用于展示与报告，
 * 因为它来自 datasheet 曲线拟合，绝对精度站不住。
 */

/* ------------------------------------------------------------------ 配置默认值 */

void Gas_DefaultCfg_MQ2(GasChannelCfg *cfg)
{
	cfg->rlOhm = 10000;          /* ⚠️ 到货后必须实测模块上的负载电阻（研究文档 §5.2） */
	cfg->vccMv = 5000;
	cfg->adcVrefMv = 3300;
	cfg->divTopOhm = 10000;      /* R10 */
	cfg->divBottomOhm = 15000;   /* R11 */
	cfg->ratioCleanAir = 9.83f;  /* MQ-2 清洁空气系数（MQUnifiedsensor Calibration.ino） */
	cfg->ppmA = 574.25f;         /* LPG（可燃气体主目标） */
	cfg->ppmB = -2.222f;
}

void Gas_DefaultCfg_MQ135(GasChannelCfg *cfg)
{
	cfg->rlOhm = 10000;
	cfg->vccMv = 5000;
	cfg->adcVrefMv = 3300;
	cfg->divTopOhm = 10000;      /* R12 */
	cfg->divBottomOhm = 15000;   /* R13 */
	cfg->ratioCleanAir = 3.6f;   /* MQ-135 清洁空气系数 */
	cfg->ppmA = 102.2f;          /* NH4（库默认示例）；若改看 CO2：a=110.47, b=-2.862 */
	cfg->ppmB = -2.473f;
}

void Gas_InitChannel(GasChannel *ch, const GasChannelCfg *cfg)
{
	uint8_t i;

	ch->cfg = *cfg;
	ch->r0 = 0.0f;
	ch->r0Sum = 0.0f;
	ch->r0Count = 0;
	ch->ratioIdx = 0;
	ch->ratioFilled = 0;
	ch->ratio = 0.0f;
	ch->ratioAvg = 0.0f;
	ch->ppm = 0.0f;
	ch->rs = 0.0f;
	ch->lastRaw = 0;
	ch->overCount = 0;
	ch->alarm = 0;
	for (i = 0; i < GAS_AVG_WINDOW; i++)
	{
		ch->ratioBuf[i] = 0.0f;
	}
}

/* ------------------------------------------------------------------ 换算 */

float Gas_AdcToVadcMv(const GasChannelCfg *cfg, uint16_t adcRaw)
{
	/* 12 位 ADC：raw / 4095 × Vref */
	return ((float)adcRaw * (float)cfg->adcVrefMv) / 4095.0f;
}

float Gas_AdcToVaoMv(const GasChannelCfg *cfg, uint16_t adcRaw)
{
	float vadc = Gas_AdcToVadcMv(cfg, adcRaw);

	/* 反分压：Vadc = Vao × Rbottom/(Rtop+Rbottom) → Vao = Vadc × (Rtop+Rbottom)/Rbottom */
	return vadc * ((float)(cfg->divTopOhm + cfg->divBottomOhm) / (float)cfg->divBottomOhm);
}

float Gas_VaoToRs(const GasChannelCfg *cfg, float vaoMv)
{
	/* Rs = (VCC × RL) / Vao − RL；Vao ≤ 0 时无意义，钳到 0 */
	if (vaoMv <= 0.0f)
	{
		return 0.0f;
	}
	{
		float rs = ((float)cfg->vccMv * (float)cfg->rlOhm) / vaoMv - (float)cfg->rlOhm;
		return (rs < 0.0f) ? 0.0f : rs;
	}
}

float Gas_CorrectionFactor(float tempC, float humiRh)
{
	/* GeorgK：factor = A·t² − B·t + C − (h−33)·D */
	const float A = 0.00035f;
	const float B = 0.02718f;
	const float C = 1.39538f;
	const float D = 0.0018f;

	return A * tempC * tempC - B * tempC + C - (humiRh - 33.0f) * D;
}

float Gas_RatioToPpm(const GasChannelCfg *cfg, float ratio)
{
	if (ratio <= 0.0f)
	{
		return 0.0f;
	}
	/* ppm = a · ratio^b（datasheet 曲线的幂律拟合） */
	return cfg->ppmA * powf(ratio, cfg->ppmB);
}

float Gas_Update(GasChannel *ch, uint16_t adcRaw, float tempC, float humiRh, int compEnable)
{
	float vao = Gas_AdcToVaoMv(&ch->cfg, adcRaw);
	float rs = Gas_VaoToRs(&ch->cfg, vao);
	float ratio;
	uint8_t i;
	float sum = 0.0f;

	ch->rs = rs;
	ch->lastRaw = adcRaw;

	if (ch->r0 <= 0.0f)
	{
		/* 未标定：ratio 无意义，只保留 Rs；ppm 记 0 */
		ch->ratio = 0.0f;
		ch->ratioAvg = 0.0f;
		ch->ppm = 0.0f;
		return 0.0f;
	}

	ratio = rs / ch->r0;

	if (compEnable)
	{
		float f = Gas_CorrectionFactor(tempC, humiRh);
		if (f > 0.01f)          /* 防止极端输入把结果放大 */
		{
			ratio = ratio / f;
		}
	}

	/* 滑动平均（环形窗口）；未填满时按已填个数平均 */
	ch->ratioBuf[ch->ratioIdx] = ratio;
	ch->ratioIdx = (uint8_t)((ch->ratioIdx + 1) % GAS_AVG_WINDOW);
	if (ch->ratioFilled < GAS_AVG_WINDOW)
	{
		ch->ratioFilled++;
	}
	for (i = 0; i < ch->ratioFilled; i++)
	{
		sum += ch->ratioBuf[i];     /* 前 ratioFilled 个即有效数据（按插入顺序即可） */
	}

	ch->ratio = ratio;
	ch->ratioAvg = sum / (float)ch->ratioFilled;
	ch->ppm = Gas_RatioToPpm(&ch->cfg, ch->ratioAvg);
	return ch->ratioAvg;
}

/* ------------------------------------------------------------------ R0 标定 */

void Gas_CalibrateAdd(GasChannel *ch, uint16_t adcRaw)
{
	float vao = Gas_AdcToVaoMv(&ch->cfg, adcRaw);
	ch->r0Sum += Gas_VaoToRs(&ch->cfg, vao);
	ch->r0Count++;
}

float Gas_CalibrateFinish(GasChannel *ch)
{
	if (ch->r0Count == 0)
	{
		return 0.0f;
	}
	{
		float rsAir = ch->r0Sum / (float)ch->r0Count;
		ch->r0 = rsAir / ch->cfg.ratioCleanAir;
		ch->r0Count = 0;
		ch->r0Sum = 0.0f;
		return ch->r0;
	}
}

/* ------------------------------------------------------------------ 阈值研判 */

int Gas_UpdateAlarm(GasChannel *ch, uint16_t threshPpmX10, uint8_t confirmFrames)
{
	/* 用 ppm×10 的整数门限与 ppm 比较（与协议 0x23 的量纲一致） */
	float thresh = (float)threshPpmX10 / 10.0f;
	float release = thresh * 0.9f;      /* 迟滞：掉到 90% 以下才解除（研究文档推荐的 90% 迟滞） */
	int rising = 0;

	if (confirmFrames == 0)
	{
		confirmFrames = 1;
	}

	if (ch->ppm > thresh)
	{
		if (ch->overCount < 0xFFFFu)
		{
			ch->overCount++;
		}
		if (!ch->alarm && ch->overCount >= confirmFrames)
		{
			ch->alarm = 1;
			rising = 1;              /* 只在"确认后的上升沿"报一次事件 */
		}
	}
	else
	{
		ch->overCount = 0;
		if (ch->alarm && ch->ppm < release)
		{
			ch->alarm = 0;           /* 迟滞解除 */
		}
	}
	return rising;
}
