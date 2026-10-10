#ifndef __RINGBUF_H
#define __RINGBUF_H

/*
 * 单生产者 / 单消费者 环形缓冲（SPSC）
 *
 * 用途（协议 §六）：**串口中断**把收到的字节推进来（生产者），
 * **CommTask** 再把字节取出去喂给协议解帧器（消费者）。
 * 这样中断里只做"存一个字节"，绝不在中断里解帧、更不调用可能阻塞的 API。
 *
 * ★ 为什么不需要锁：
 *   · 只有一个生产者、一个消费者；
 *   · 只读写各自的索引（head 仅生产者改、tail 仅消费者改），
 *     且在 Cortex-M4 上对齐的 16 位读写是**单指令原子**的；
 *   · 缓冲满时**丢弃新字节**并计数（宁可丢最旧的通信数据，也不能覆盖未读数据）；
 *   · 需要"先看再取"的地方，用 tail/head 的快照，允许乐观重试。
 *
 * ★ 容量必须是 2 的幂（用位与代替取模）；实存 capacity-1 个字节，
 *   用一个空槽区分"满"和"空"。
 *
 * ★ 只依赖 <stdint.h> —— 可在 PC 上原生编译做单元测试。
 */

#include <stdint.h>

#define RINGBUF_CAPACITY 512u   /* 2 的幂；蓝牙 115200 下约 45 ms 的缓冲 */

typedef struct
{
	volatile uint16_t head;      /* 生产者写 */
	volatile uint16_t tail;      /* 消费者写 */
	volatile uint32_t dropped;   /* 因满而丢弃的字节数（诊断用） */
	uint8_t  buf[RINGBUF_CAPACITY];
} RingBuf;

void     RingBuf_Init(RingBuf *rb);
uint16_t RingBuf_Count(const RingBuf *rb);
uint16_t RingBuf_Free(const RingBuf *rb);
/* 中断里调用：成功返回 1；满则丢弃并返回 0 */
int      RingBuf_Push(RingBuf *rb, uint8_t byte);
/* 中断里调用（一次推多个，减少索引更新次数）：返回成功推入的个数 */
uint16_t RingBuf_PushBuf(RingBuf *rb, const uint8_t *data, uint16_t len);
/* 任务里调用：成功返回 1 */
int      RingBuf_Pop(RingBuf *rb, uint8_t *byte);
/* 任务里调用：一次取多个，返回取到的个数 */
uint16_t RingBuf_PopBuf(RingBuf *rb, uint8_t *out, uint16_t maxLen);

#endif /* __RINGBUF_H */
