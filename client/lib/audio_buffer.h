#ifndef CLIENT_LIB_AUDIO_BUFFER_H
#define CLIENT_LIB_AUDIO_BUFFER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *samples;
  size_t capacity_frames;
  size_t buffered_frames;
  size_t frame_bytes;
  size_t total_frames;
  double position_frames;
  double speed;
  int paused;
  int complete;
  pthread_mutex_t lock;
} AudioBuffer;

typedef struct {
  double position_frames;
  size_t buffered_frames;
  size_t total_frames;
  double speed;
  int paused;
  int complete;
} AudioBufferState;

int audio_buffer_init(AudioBuffer *buffer);
void audio_buffer_destroy(AudioBuffer *buffer);
int audio_buffer_set_format(AudioBuffer *buffer, size_t frame_bytes,
                            uint64_t total_frames);
int audio_buffer_append(AudioBuffer *buffer, const uint8_t *data, size_t len);
void audio_buffer_finish(AudioBuffer *buffer);
void audio_buffer_set_paused(AudioBuffer *buffer, int paused);
void audio_buffer_toggle_paused(AudioBuffer *buffer);
void audio_buffer_adjust_speed(AudioBuffer *buffer, int direction);
void audio_buffer_seek(AudioBuffer *buffer, int64_t frame_delta);
void audio_buffer_render(AudioBuffer *buffer, int16_t *output,
                         size_t output_frames, size_t channels);
void audio_buffer_snapshot(AudioBuffer *buffer, AudioBufferState *state);
int audio_buffer_is_finished(AudioBuffer *buffer);

#endif
