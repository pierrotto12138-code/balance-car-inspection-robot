#include "ringbuf.h"

/* 见 ringbuf.h 的说明：SPSC，无需关中断；索引对齐 16 位，读写原子。 */

void RingBuf_Init(RingBuf *rb)
{
	if (rb == 0)
	{
		return;
	}
	rb->head = 0;
	rb->tail = 0;
	rb->dropped = 0;
}

uint16_t RingBuf_Count(const RingBuf *rb)
{
	uint16_t head = rb->head;
	uint16_t tail = rb->tail;
	return (uint16_t)((head - tail) & (RINGBUF_CAPACITY - 1u));
}

uint16_t RingBuf_Free(const RingBuf *rb)
{
	/* 留一个空槽，故可用空间 = 容量-1-已存 */
	return (uint16_t)((RINGBUF_CAPACITY - 1u) - RingBuf_Count(rb));
}

int RingBuf_Push(RingBuf *rb, uint8_t byte)
{
	uint16_t head = rb->head;
	uint16_t next = (uint16_t)((head + 1u) & (RINGBUF_CAPACITY - 1u));

	if (next == rb->tail)
	{
		rb->dropped++;          /* 满：丢新字节 */
		return 0;
	}
	rb->buf[head] = byte;
	rb->head = next;            /* 最后再发布索引，保证消费者看到的是完整数据 */
	return 1;
}

uint16_t RingBuf_PushBuf(RingBuf *rb, const uint8_t *data, uint16_t len)
{
	uint16_t i;
	for (i = 0; i < len; i++)
	{
		if (!RingBuf_Push(rb, data[i]))
		{
			break;
		}
	}
	return i;
}

int RingBuf_Pop(RingBuf *rb, uint8_t *byte)
{
	uint16_t tail = rb->tail;

	if (tail == rb->head)
	{
		return 0;               /* 空 */
	}
	*byte = rb->buf[tail];
	rb->tail = (uint16_t)((tail + 1u) & (RINGBUF_CAPACITY - 1u));
	return 1;
}

uint16_t RingBuf_PopBuf(RingBuf *rb, uint8_t *out, uint16_t maxLen)
{
	uint16_t n = 0;
	while (n < maxLen && RingBuf_Pop(rb, &out[n]))
	{
		n++;
	}
	return n;
}
