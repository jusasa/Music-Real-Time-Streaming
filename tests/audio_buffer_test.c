#include "audio_buffer.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
  AudioBuffer buffer;
  assert(audio_buffer_init(&buffer) == 0);
  assert(audio_buffer_set_format(&buffer, sizeof(int16_t), 10) == 0);

  int16_t samples[10];
  for (int16_t i = 0; i < 10; i++)
    samples[i] = i * 1000;

  assert(audio_buffer_append(&buffer, (const uint8_t *)samples,
                             4 * sizeof(int16_t)) == 0);
  audio_buffer_seek(&buffer, 100);
  AudioBufferState state;
  audio_buffer_snapshot(&buffer, &state);
  assert(state.position_frames == 3);
  assert(state.buffered_frames == 4);

  audio_buffer_seek(&buffer, -100);
  int16_t output[2];
  audio_buffer_render(&buffer, output, 2, 1);
  assert(output[0] == 0);
  assert(output[1] == 1000);

  for (int i = 0; i < 4; i++)
    audio_buffer_adjust_speed(&buffer, 1);
  audio_buffer_render(&buffer, output, 1, 1);
  assert(output[0] == 2000);

  audio_buffer_render(&buffer, output, 1, 1);
  assert(output[0] == 0);
  audio_buffer_snapshot(&buffer, &state);
  assert(state.position_frames == 4);

  assert(audio_buffer_append(&buffer, (const uint8_t *)&samples[4],
                             6 * sizeof(int16_t)) == 0);
  for (int i = 0; i < 6; i++)
    audio_buffer_adjust_speed(&buffer, -1);
  audio_buffer_render(&buffer, output, 2, 1);
  assert(output[0] == 4000);
  assert(output[1] == 4500);

  audio_buffer_toggle_paused(&buffer);
  audio_buffer_snapshot(&buffer, &state);
  double paused_position = state.position_frames;
  output[0] = 123;
  audio_buffer_render(&buffer, output, 1, 1);
  assert(output[0] == 0);
  audio_buffer_snapshot(&buffer, &state);
  assert(state.paused);
  assert(state.position_frames == paused_position);

  for (int i = 0; i < 100; i++)
    audio_buffer_adjust_speed(&buffer, 1);
  audio_buffer_snapshot(&buffer, &state);
  assert(state.speed == 4.0);
  for (int i = 0; i < 100; i++)
    audio_buffer_adjust_speed(&buffer, -1);
  audio_buffer_snapshot(&buffer, &state);
  assert(state.speed == 0.5);

  audio_buffer_seek(&buffer, 100);
  assert(audio_buffer_is_finished(&buffer));
  audio_buffer_destroy(&buffer);

  AudioBuffer stereo;
  assert(audio_buffer_init(&stereo) == 0);
  assert(audio_buffer_set_format(&stereo, 2 * sizeof(int16_t), 2) == 0);
  int16_t stereo_samples[] = {1000, -1000, 2000, -2000};
  int16_t stereo_output[4];
  assert(audio_buffer_append(&stereo, (const uint8_t *)stereo_samples,
                             sizeof(stereo_samples)) == 0);
  audio_buffer_render(&stereo, stereo_output, 2, 2);
  assert(stereo_output[0] == 1000 && stereo_output[1] == -1000);
  assert(stereo_output[2] == 2000 && stereo_output[3] == -2000);
  audio_buffer_destroy(&stereo);
  return 0;
}
