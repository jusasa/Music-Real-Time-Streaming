/*
 * audio_player.c - PortAudio 기반 오디오 재생 구현
 */

#include "audio_player.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int pa_callback(const void *input, void *output,
                       unsigned long frame_count,
                       const PaStreamCallbackTimeInfo *time_info,
                       PaStreamCallbackFlags status_flags, void *user_data)
{
    (void)input;
    (void)time_info;
    (void)status_flags;

    AudioPlayer *ap = (AudioPlayer *)user_data;
    uint8_t *out = (uint8_t *)output;
    size_t bytes_per_frame = (size_t)ap->meta.channels * (ap->meta.bits_per_sample / 8);
    size_t want = (size_t)frame_count * bytes_per_frame;

    if (ap->paused)
    {
        memset(out, 0, want); /* 일시정지 중에는 무음 출력 */
        return paContinue;
    }

    size_t got = ring_read(ap->ring, out, want);
    if (got < want)
    {
        /* 언더런: 남은 부분은 무음으로 채워 지직거림 방지 */
        memset(out + got, 0, want - got);
    }
    return paContinue;
}

int audio_player_start(AudioPlayer *ap, RingBuffer *ring, const AudioMetaPayload *meta)
{
    ap->ring = ring;
    ap->meta = *meta;
    ap->stream = NULL;
    ap->paused = 0;

    PaError err = Pa_Initialize();
    if (err != paNoError)
    {
        fprintf(stderr, "[client] Pa_Initialize 실패: %s\n", Pa_GetErrorText(err));
        return -1;
    }

    err = Pa_OpenDefaultStream(
        &ap->stream,
        0, /* 입력 채널 없음 */
        ap->meta.channels,
        paInt16,
        ap->meta.sample_rate,
        PA_FRAMES_PER_BUFFER,
        pa_callback,
        ap);
    if (err != paNoError)
    {
        fprintf(stderr, "[client] Pa_OpenDefaultStream 실패: %s\n", Pa_GetErrorText(err));
        Pa_Terminate();
        return -1;
    }

    err = Pa_StartStream(ap->stream);
    if (err != paNoError)
    {
        fprintf(stderr, "[client] Pa_StartStream 실패: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(ap->stream);
        ap->stream = NULL;
        Pa_Terminate();
        return -1;
    }
    return 0;
}

void audio_player_stop(AudioPlayer *ap)
{
    if (ap->stream)
    {
        Pa_StopStream(ap->stream);
        Pa_CloseStream(ap->stream);
        ap->stream = NULL;
    }
    Pa_Terminate();
}

void audio_player_set_paused(AudioPlayer *ap, int paused)
{
    ap->paused = paused;
}
