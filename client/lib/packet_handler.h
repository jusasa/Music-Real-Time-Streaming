/*
 * packet_handler.h - 서버 패킷 수신/처리 루프
 */

#ifndef CLIENT_LIB_PACKET_HANDLER_H
#define CLIENT_LIB_PACKET_HANDLER_H

#include <pthread.h>

#include "audio_player.h"
#include "protocol.h"
#include "ring_buffer.h"

/* 수신 루프가 공유 상태에 접근하기 위한 컨텍스트 */
typedef struct
{
    int fd;                   /* 서버 소켓 */
    RingBuffer *ring;         /* 오디오 데이터 적재 대상 */
    AudioPlayer *player;      /* 재생 제어 대상 */
    pthread_mutex_t *player_lock;
    int paused;
    AudioMetaPayload *meta;   /* 수신한 오디오 포맷 저장소 */
    volatile int *meta_ready; /* 메타데이터 수신 완료 플래그 */
    volatile int *running;    /* 전체 실행 플래그 */
} PacketContext;

/* 수신 스레드 진입점. 종료 시 링 버퍼에 EOF를 통지한다 */
void *packet_recv_loop(void *arg);

#endif /* CLIENT_LIB_PACKET_HANDLER_H */
