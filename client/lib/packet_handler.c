/*
 * packet_handler.c - 서버 패킷 수신/처리 루프 구현
 */

#include "packet_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "net_util.h"

/* 비정상 대용량 페이로드 방지 상한 (4 MiB) */
#define MAX_PAYLOAD (4 * 1024 * 1024)

static void set_player_paused(PacketContext *ctx, int paused)
{
    pthread_mutex_lock(ctx->player_lock);
    ctx->paused = paused;
    if (ctx->player)
        audio_player_set_paused(ctx->player, paused);
    pthread_mutex_unlock(ctx->player_lock);
}

static void handle_control(PacketContext *ctx, const uint8_t *payload, uint32_t len)
{
    if (len < 1)
        return;

    switch (payload[0])
    {
    case CTRL_PLAY:
        set_player_paused(ctx, 0);
        printf("[client] ▶ PLAY\n");
        break;
    case CTRL_PAUSE:
        set_player_paused(ctx, 1);
        printf("[client] ⏸ PAUSE\n");
        break;
    case CTRL_STOP:
        printf("[client] ⏹ STOP\n");
        *ctx->running = 0;
        break;
    default:
        printf("[client] 알 수 없는 제어 명령: %u\n", payload[0]);
        break;
    }
}

void *packet_recv_loop(void *arg)
{
    PacketContext *ctx = (PacketContext *)arg;

    while (*ctx->running)
    {
        PacketHeader hdr;
        if (recv_all(ctx->fd, &hdr, sizeof(hdr)) != 0)
        {
            printf("[client] 서버 연결이 종료되었습니다.\n");
            *ctx->running = 0;
            break;
        }

        /* 매직 넘버 검증 */
        if (hdr.magic != PACKET_MAGIC)
        {
            fprintf(stderr, "[client] 잘못된 패킷 (magic=0x%08X). 연결 종료.\n",
                    hdr.magic);
            *ctx->running = 0;
            break;
        }

        /* 페이로드 크기 상한 검증 */
        if (hdr.payload_len > MAX_PAYLOAD)
        {
            fprintf(stderr, "[client] 비정상 페이로드 크기: %u\n", hdr.payload_len);
            *ctx->running = 0;
            break;
        }

        uint8_t *payload = NULL;
        if (hdr.payload_len > 0)
        {
            payload = (uint8_t *)malloc(hdr.payload_len);
            if (!payload)
            {
                perror("malloc(payload)");
                *ctx->running = 0;
                break;
            }
            if (recv_all(ctx->fd, payload, hdr.payload_len) != 0)
            {
                free(payload);
                *ctx->running = 0;
                break;
            }
        }

        switch (hdr.type)
        {
        case PKT_AUDIO_INIT:
            if (hdr.payload_len >= sizeof(AudioMetaPayload))
            {
                memcpy(ctx->meta, payload, sizeof(AudioMetaPayload));
                if (ctx->meta->bits_per_sample != 16 ||
                    ctx->meta->channels == 0 ||
                    ctx->meta->sample_rate == 0)
                {
                    fprintf(stderr, "[client] 지원하지 않는 오디오 포맷입니다.\n");
                    *ctx->running = 0;
                    break;
                }
                size_t frame_bytes =
                    ctx->meta->channels * (ctx->meta->bits_per_sample / 8);
                if (audio_buffer_set_format(ctx->buffer, frame_bytes,
                                            ctx->meta->total_frames) != 0)
                {
                    perror("audio_buffer_set_format");
                    *ctx->running = 0;
                    break;
                }
                *ctx->meta_ready = 1;
                printf("[client] 오디오 포맷: %u Hz, %u ch, %u bit\n",
                       ctx->meta->sample_rate, ctx->meta->channels,
                       ctx->meta->bits_per_sample);
            }
            else
            {
                fprintf(stderr, "[client] 오디오 메타데이터가 잘못되었습니다.\n");
                *ctx->running = 0;
            }
            break;

        case PKT_AUDIO_DATA:
            if (hdr.payload_len > 0)
            {
                if (audio_buffer_append(ctx->buffer, payload,
                                        hdr.payload_len) != 0)
                {
                    perror("audio_buffer_append");
                    *ctx->running = 0;
                }
            }
            break;

        case PKT_CONTROL:
            handle_control(ctx, payload, hdr.payload_len);
            break;

        default:
            fprintf(stderr, "[client] 알 수 없는 패킷 타입: %u\n", hdr.type);
            break;
        }

        free(payload);
    }

    audio_buffer_finish(ctx->buffer);
    return NULL;
}
