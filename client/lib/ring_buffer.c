/*
 * ring_buffer.c - 스레드 안전 링 버퍼 구현
 */

#include "ring_buffer.h"

#include <stdlib.h>
#include <string.h>

int ring_init(RingBuffer *r, size_t capacity)
{
    r->buf = (uint8_t *)malloc(capacity);
    if (!r->buf)
        return -1;

    r->capacity = capacity;
    r->read_pos = 0;
    r->write_pos = 0;
    r->count = 0;
    r->eof = 0;

    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->not_empty, NULL);
    pthread_cond_init(&r->not_full, NULL);
    return 0;
}

void ring_destroy(RingBuffer *r)
{
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->not_empty);
    pthread_cond_destroy(&r->not_full);
    free(r->buf);
    r->buf = NULL;
}

void ring_write(RingBuffer *r, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&r->lock);

    size_t written = 0;
    while (written < len)
    {
        while (r->count == r->capacity && !r->eof)
            pthread_cond_wait(&r->not_full, &r->lock);

        if (r->eof)
            break; /* 종료 중이면 남은 데이터는 버린다 */

        size_t space = r->capacity - r->count;
        size_t chunk = len - written;
        if (chunk > space)
            chunk = space;

        /* 링 경계를 넘는 경우 두 번에 나눠 복사 */
        size_t first = r->capacity - r->write_pos;
        if (first > chunk)
            first = chunk;
        memcpy(r->buf + r->write_pos, data + written, first);
        if (chunk > first)
            memcpy(r->buf, data + written + first, chunk - first);

        r->write_pos = (r->write_pos + chunk) % r->capacity;
        r->count += chunk;
        written += chunk;

        pthread_cond_signal(&r->not_empty);
    }

    pthread_mutex_unlock(&r->lock);
}

size_t ring_read(RingBuffer *r, uint8_t *out, size_t len)
{
    pthread_mutex_lock(&r->lock);

    while (r->count == 0 && !r->eof)
        pthread_cond_wait(&r->not_empty, &r->lock);

    size_t chunk = len;
    if (chunk > r->count)
        chunk = r->count;

    size_t first = r->capacity - r->read_pos;
    if (first > chunk)
        first = chunk;
    memcpy(out, r->buf + r->read_pos, first);
    if (chunk > first)
        memcpy(out + first, r->buf, chunk - first);

    r->read_pos = (r->read_pos + chunk) % r->capacity;
    r->count -= chunk;

    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->lock);
    return chunk;
}

void ring_set_eof(RingBuffer *r)
{
    pthread_mutex_lock(&r->lock);
    r->eof = 1;
    pthread_cond_broadcast(&r->not_empty);
    pthread_cond_broadcast(&r->not_full);
    pthread_mutex_unlock(&r->lock);
}
