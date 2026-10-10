#include "sensor_report.h"

/* 见 sensor_report.h：这里只做"换算 + 钳位 + 组包"，不碰硬件。 */

uint16_t SensorReport_PpmToX10(float ppm)
{
	float v;

	if (ppm <= 0.0f)
	{
		return 0;
	}
	v = ppm * 10.0f;
	if (v >= (float)SENSOR_PPM_X10_MAX)
	{
		return (uint16_t)SENSOR_PPM_X10_MAX;     /* 饱和：宁可顶格，也不要回绕 */
	}
	return (uint16_t)(v + 0.5f);                 /* 四舍五入 */
}

void SensorReport_FillEnvGas(const GasChannel *mq2, const GasChannel *mq135,
                             const Dht11Reading *dht, int dhtValid,
                             int compEnable, uint8_t gear,
                             ProtoEnvGas *out)
{
	uint8_t ready = 0;

	out->mq2Raw = 0;
	out->mq135Raw = 0;
	out->mq2PpmX10 = 0;
	out->mq135PpmX10 = 0;
	out->tempX10 = 0;
	out->humiX10 = 0;

	if (mq2 != 0)
	{
		out->mq2Raw = mq2->lastRaw;
		out->mq2PpmX10 = SensorReport_PpmToX10(mq2->ppm);
		if (mq2->r0 > 0.0f)
		{
			ready |= PROTO_RDY_MQ2;              /* 标定完成才算"就绪" */
		}
	}
	if (mq135 != 0)
	{
		out->mq135Raw = mq135->lastRaw;
		out->mq135PpmX10 = SensorReport_PpmToX10(mq135->ppm);
		if (mq135->r0 > 0.0f)
		{
			ready |= PROTO_RDY_MQ135;
		}
	}
	if (dhtValid && dht != 0)
	{
		ready |= PROTO_RDY_DHT11;
		out->tempX10 = (int16_t)(Dht11_TempC(dht) * 10.0f + 0.5f);
		out->humiX10 = (uint16_t)(Dht11_HumiRh(dht) * 10.0f + 0.5f);
	}
	if (compEnable && (ready & PROTO_RDY_DHT11))
	{
		ready |= PROTO_RDY_COMP;                 /* 只有 DHT 数据有效时补偿才真的生效 */
	}

	out->readyBits = ready;
	out->gear = gear;
}

uint8_t SensorReport_DecideGear(const GasChannel *mq2, const GasChannel *mq135,
                                int warmupDone, int adaptive)
{
	if (!warmupDone)
	{
		return PROTO_GEAR_WARMUP;
	}
	if (!adaptive)
	{
		return PROTO_GEAR_NORMAL;
	}
	if ((mq2 != 0 && (mq2->alarm || mq2->overCount > 0)) ||
	    (mq135 != 0 && (mq135->alarm || mq135->overCount > 0)))
	{
		return PROTO_GEAR_FAST;                  /* 异常驱动：1 Hz → 5 Hz */
	}
	return PROTO_GEAR_NORMAL;
}
