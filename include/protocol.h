#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define SERVER_PORT 8080
#define PACKET_MAGIC 0x53594E43 // 'SYNC'

// 패킷 타입
#define PKT_AUDIO_INIT 1 // 오디오 메타데이터 (샘플레이트, 채널 등)
#define PKT_AUDIO_DATA 2 // 실제 PCM 오디오 청크
#define PKT_CONTROL 3    // PLAY, PAUSE 등

#pragma pack(push, 1)
typedef struct
{
    uint32_t magic;        // 패킷 검증용 매직 넘버
    uint16_t type;         // 패킷 타입
    uint32_t payload_len;  // 뒤따르는 바이트 크기
    uint64_t timestamp_ms; // 서버 기준 타임스탬프
} PacketHeader;

typedef struct
{
    uint32_t sample_rate;     // 예: 44100
    uint16_t channels;        // 예: 2 (스테레오)
    uint16_t bits_per_sample; // 예: 16 (S16LE)
    uint64_t total_frames;    // 전체 PCM 프레임 수
} AudioMetaPayload;
#pragma pack(pop)

#endif