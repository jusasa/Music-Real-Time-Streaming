/*
 * audio_player.c - PortAudio 기반 오디오 재생 구현
 */

#include "audio_player.h"

#include <stdio.h>

static int pa_callback(const void *input, void *output,
                       unsigned long frame_count,
                       const PaStreamCallbackTimeInfo *time_info,
                       PaStreamCallbackFlags status_flags, void *user_data)
{
    (void)input;
    (void)time_info;
    (void)status_flags;

    AudioPlayer *ap = (AudioPlayer *)user_data;
    audio_buffer_render(ap->buffer, output, frame_count, ap->meta.channels);
    return paContinue;
}

int audio_player_start(AudioPlayer *ap, AudioBuffer *buffer,
                       const AudioMetaPayload *meta)
{
    if (meta->bits_per_sample != 16 || meta->channels == 0) {
        fprintf(stderr, "[client] 지원하지 않는 오디오 포맷입니다.\n");
        return -1;
    }

    ap->buffer = buffer;
    ap->meta = *meta;
    ap->stream = NULL;

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
    audio_buffer_set_paused(ap->buffer, paused);
}
