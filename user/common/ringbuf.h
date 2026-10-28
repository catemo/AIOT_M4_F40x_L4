/**
 * @file    ringbuf.h
 * @brief   无锁单生产者/单消费者环形缓冲（common 横切层）
 *
 * 典型用法：UART ISR（生产者）写入，任务循环（消费者）读出。
 * 约束：容量必须是 2 的幂（CFG_RINGBUF_SIZE）；SPSC 场景下读写无需关中断。
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include <stddef.h>
#include "module_cfg.h"

typedef struct {
    volatile uint32_t head;   /* 写指针（仅生产者改） */
    volatile uint32_t tail;   /* 读指针（仅消费者改） */
    uint32_t          mask;   /* size-1，配合 2 的幂容量做取模 */
    uint8_t           buf[CFG_RINGBUF_SIZE];
} ringbuf_t;

void   rb_init(ringbuf_t *rb);
size_t rb_write(ringbuf_t *rb, const uint8_t *data, size_t len); /* 返回实际写入数 */
size_t rb_read(ringbuf_t *rb, uint8_t *out, size_t cap);         /* 返回实际读出数 */
size_t rb_count(const ringbuf_t *rb);                            /* 可读字节数     */
size_t rb_space(const ringbuf_t *rb);                            /* 剩余空位       */

#endif /* RINGBUF_H */
