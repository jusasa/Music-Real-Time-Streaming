/*
 * ring_buffer.h - 수신 스레드와 오디오 콜백을 잇는 스레드 안전 링 버퍼
 */

#ifndef CLIENT_LIB_RING_BUFFER_H
#define CLIENT_LIB_RING_BUFFER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint8_t *buf;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t count; /* 현재 저장된 바이트 수 */

    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    int eof; /* 수신 종료 플래그 */
} RingBuffer;

/* 버퍼 할당 및 동기화 객체 초기화. 성공 시 0, 실패 시 -1 */
int ring_init(RingBuffer *r, size_t capacity);

/* 자원 해제 */
void ring_destroy(RingBuffer *r);

/* 데이터 적재. 버퍼가 가득 차면 공간이 생길 때까지 대기 */
void ring_write(RingBuffer *r, const uint8_t *data, size_t len);

/* 최대 len 바이트 읽기. 실제 읽은 바이트 수 반환 */
size_t ring_read(RingBuffer *r, uint8_t *out, size_t len);

/* 수신 종료 통지: 대기 중인 스레드를 모두 깨운다 */
void ring_set_eof(RingBuffer *r);

#endif /* CLIENT_LIB_RING_BUFFER_H */
