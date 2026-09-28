/*
 * audio_player.h - PortAudio 기반 오디오 재생 모듈
 */

#ifndef CLIENT_LIB_AUDIO_PLAYER_H
#define CLIENT_LIB_AUDIO_PLAYER_H

#include <portaudio.h>

#include "protocol.h"
#include "ring_buffer.h"

/* PortAudio 콜백에서 사용할 프레임 단위 버퍼 */
#define PA_FRAMES_PER_BUFFER 1024

typedef struct
{
    RingBuffer *ring;      /* 재생할 PCM 데이터 소스 */
    AudioMetaPayload meta; /* 오디오 포맷 */
    PaStream *stream;      /* PortAudio 스트림 */
    volatile int paused;   /* 일시정지 여부 */
} AudioPlayer;

/* 스트림을 열고 재생을 시작한다. 성공 시 0, 실패 시 -1 */
int audio_player_start(AudioPlayer *ap, RingBuffer *ring, const AudioMetaPayload *meta);

/* 스트림을 정지/해제하고 PortAudio를 종료한다 */
void audio_player_stop(AudioPlayer *ap);

/* 일시정지 상태 설정 (1: 일시정지, 0: 재생) */
void audio_player_set_paused(AudioPlayer *ap, int paused);

#endif /* CLIENT_LIB_AUDIO_PLAYER_H */
