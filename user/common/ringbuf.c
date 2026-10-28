/**
 * @file    ringbuf.c
 * @brief   无锁单生产者/单消费者环形缓冲实现
 *
 * 正确性依赖：head/tail 各自只有一个写者，uint32_t 读写在 Cortex-M4 上原子。
 * 满/空判定：统一放弃一个空位（tail+1 == head 视为满），实现简单且无歧义。
 */
#include "ringbuf.h"

void rb_init(ringbuf_t *rb)
{
    rb->head = 0u;
    rb->tail = 0u;
    rb->mask = (uint32_t)(CFG_RINGBUF_SIZE - 1u);
}

size_t rb_space(const ringbuf_t *rb)
{
    /* head - tail 恒为正（无符号回绕天然正确），满时空 1 位 */
    return (size_t)(rb->mask - (rb->head - rb->tail));
}

size_t rb_count(const ringbuf_t *rb)
{
    return (size_t)(rb->head - rb->tail);
}

size_t rb_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    size_t space = rb_space(rb);
    size_t n;

    if (len > space) {
        len = space;                 /* 放不下就截断，绝不阻塞、绝不覆盖 */
    }
    for (n = 0u; n < len; n++) {
        rb->buf[rb->head & rb->mask] = data[n];
        rb->head++;                  /* 先写数据后推进指针（SPSC 顺序保证） */
    }
    return len;
}

size_t rb_read(ringbuf_t *rb, uint8_t *out, size_t cap)
{
    size_t cnt = rb_count(rb);
    size_t n;

    if (cap > cnt) {
        cap = cnt;
    }
    for (n = 0u; n < cap; n++) {
        out[n] = rb->buf[rb->tail & rb->mask];
        rb->tail++;                  /* 先取数据后推进指针 */
    }
    return cap;
}
