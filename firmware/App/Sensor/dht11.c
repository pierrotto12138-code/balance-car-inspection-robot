#include "dht11.h"

/* 见 dht11.h：只做"数据可信度判断 + 换算"，不碰硬件。 */

int Dht11_Parse(const uint8_t raw[5], Dht11Reading *out)
{
	uint8_t sum = 0;
	uint8_t i;

	for (i = 0; i < 4; i++)
	{
		sum = (uint8_t)(sum + raw[i]);
	}

	/* 典型故障特征：总线没接（全 0）或一直拉高（全 0xFF） */
	if ((raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0) ||
		(raw[0] == 0xFF && raw[1] == 0xFF && raw[2] == 0xFF && raw[3] == 0xFF))
	{
		return -3;
	}

	if (sum != raw[4])
	{
		return -1;              /* 校验失败：整帧丢弃，不要"用一半" */
	}

	/* DHT11 量程：湿度 20~90 %RH（留宽裕），温度 0~50 ℃（留宽裕到 60） */
	if (raw[0] > 100 || raw[2] > 60)
	{
		return -2;
	}

	out->humiInt = raw[0];
	out->humiDec = raw[1];
	out->tempInt = raw[2];
	out->tempDec = raw[3];
	return 0;
}

float Dht11_TempC(const Dht11Reading *r)
{
	return (float)r->tempInt + (float)r->tempDec * 0.1f;
}

float Dht11_HumiRh(const Dht11Reading *r)
{
	return (float)r->humiInt + (float)r->humiDec * 0.1f;
}
