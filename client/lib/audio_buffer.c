#include "audio_buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MIN_SPEED 0.5
#define MAX_SPEED 4.0
#define SPEED_STEP 0.25
#define INITIAL_CAPACITY_FRAMES 4096

static int16_t read_sample(const uint8_t *data, size_t frame_bytes,
                           size_t frame, size_t channel) {
  int16_t sample;
  memcpy(&sample, data + frame * frame_bytes + channel * sizeof(sample),
         sizeof(sample));
  return sample;
}

int audio_buffer_init(AudioBuffer *buffer) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->speed = 1.0;
  int err = pthread_mutex_init(&buffer->lock, NULL);
  if (err != 0) {
    errno = err;
    return -1;
  }
  return 0;
}

void audio_buffer_destroy(AudioBuffer *buffer) {
  pthread_mutex_destroy(&buffer->lock);
  free(buffer->samples);
  buffer->samples = NULL;
}

int audio_buffer_set_format(AudioBuffer *buffer, size_t frame_bytes,
                            uint64_t total_frames) {
  if (frame_bytes == 0 || total_frames > SIZE_MAX / frame_bytes) {
    errno = EOVERFLOW;
    return -1;
  }

  pthread_mutex_lock(&buffer->lock);
  buffer->frame_bytes = frame_bytes;
  buffer->total_frames = (size_t)total_frames;
  buffer->complete = total_frames == 0;
  pthread_mutex_unlock(&buffer->lock);
  return 0;
}

int audio_buffer_append(AudioBuffer *buffer, const uint8_t *data, size_t len) {
  pthread_mutex_lock(&buffer->lock);

  if (buffer->frame_bytes == 0 || len % buffer->frame_bytes != 0) {
    pthread_mutex_unlock(&buffer->lock);
    errno = EINVAL;
    return -1;
  }

  size_t frames = len / buffer->frame_bytes;
  if (frames > buffer->total_frames - buffer->buffered_frames) {
    pthread_mutex_unlock(&buffer->lock);
    errno = EOVERFLOW;
    return -1;
  }

  size_t needed = buffer->buffered_frames + frames;
  if (needed > buffer->capacity_frames) {
    size_t capacity = buffer->capacity_frames;
    if (capacity == 0) {
      capacity = buffer->total_frames < INITIAL_CAPACITY_FRAMES
                     ? buffer->total_frames
                     : INITIAL_CAPACITY_FRAMES;
    }
    while (capacity < needed) {
      if (capacity > buffer->total_frames / 2) {
        capacity = buffer->total_frames;
        break;
      }
      capacity *= 2;
    }

    if (capacity > SIZE_MAX / buffer->frame_bytes) {
      pthread_mutex_unlock(&buffer->lock);
      errno = EOVERFLOW;
      return -1;
    }
    uint8_t *samples =
        realloc(buffer->samples, capacity * buffer->frame_bytes);
    if (!samples) {
      pthread_mutex_unlock(&buffer->lock);
      return -1;
    }
    buffer->samples = samples;
    buffer->capacity_frames = capacity;
  }

  memcpy(buffer->samples + buffer->buffered_frames * buffer->frame_bytes, data,
         len);
  buffer->buffered_frames = needed;
  if (buffer->buffered_frames == buffer->total_frames)
    buffer->complete = 1;
  pthread_mutex_unlock(&buffer->lock);
  return 0;
}

void audio_buffer_finish(AudioBuffer *buffer) {
  pthread_mutex_lock(&buffer->lock);
  buffer->total_frames = buffer->buffered_frames;
  buffer->complete = 1;
  if (buffer->position_frames > buffer->total_frames)
    buffer->position_frames = buffer->total_frames;
  pthread_mutex_unlock(&buffer->lock);
}

void audio_buffer_set_paused(AudioBuffer *buffer, int paused) {
  pthread_mutex_lock(&buffer->lock);
  buffer->paused = paused;
  pthread_mutex_unlock(&buffer->lock);
}

void audio_buffer_toggle_paused(AudioBuffer *buffer) {
  pthread_mutex_lock(&buffer->lock);
  buffer->paused = !buffer->paused;
  pthread_mutex_unlock(&buffer->lock);
}

void audio_buffer_adjust_speed(AudioBuffer *buffer, int direction) {
  pthread_mutex_lock(&buffer->lock);
  if (direction > 0)
    buffer->speed += SPEED_STEP;
  else if (direction < 0)
    buffer->speed -= SPEED_STEP;
  if (buffer->speed < MIN_SPEED)
    buffer->speed = MIN_SPEED;
  if (buffer->speed > MAX_SPEED)
    buffer->speed = MAX_SPEED;
  pthread_mutex_unlock(&buffer->lock);
}

void audio_buffer_seek(AudioBuffer *buffer, int64_t frame_delta) {
  pthread_mutex_lock(&buffer->lock);
  double max_position = buffer->buffered_frames;
  if (max_position > 0 && !buffer->complete)
    max_position -= 1;

  double position = buffer->position_frames + (double)frame_delta;
  if (position < 0)
    position = 0;
  if (position > max_position)
    position = max_position;
  buffer->position_frames = position;
  pthread_mutex_unlock(&buffer->lock);
}

void audio_buffer_render(AudioBuffer *buffer, int16_t *output,
                         size_t output_frames, size_t channels) {
  size_t frame_bytes = channels * sizeof(int16_t);
  memset(output, 0, output_frames * frame_bytes);

  pthread_mutex_lock(&buffer->lock);
  if (buffer->paused) {
    pthread_mutex_unlock(&buffer->lock);
    return;
  }

  double position = buffer->position_frames;
  for (size_t frame = 0; frame < output_frames; frame++) {
    if (position >= buffer->buffered_frames ||
        position >= buffer->total_frames)
      break;

    size_t source_frame = (size_t)position;
    size_t next_frame = source_frame + 1;
    if (next_frame >= buffer->buffered_frames)
      next_frame = source_frame;
    double fraction = position - source_frame;

    for (size_t channel = 0; channel < channels; channel++) {
      double first =
          read_sample(buffer->samples, frame_bytes, source_frame, channel);
      double second =
          read_sample(buffer->samples, frame_bytes, next_frame, channel);
      double sample = first + (second - first) * fraction;
      if (sample > INT16_MAX)
        sample = INT16_MAX;
      else if (sample < INT16_MIN)
        sample = INT16_MIN;
      output[frame * channels + channel] = (int16_t)sample;
    }

    position += buffer->speed;
    if (!buffer->complete && position > buffer->buffered_frames)
      position = buffer->buffered_frames;
  }
  buffer->position_frames = position;
  pthread_mutex_unlock(&buffer->lock);
}

void audio_buffer_snapshot(AudioBuffer *buffer, AudioBufferState *state) {
  pthread_mutex_lock(&buffer->lock);
  state->position_frames = buffer->position_frames;
  state->buffered_frames = buffer->buffered_frames;
  state->total_frames = buffer->total_frames;
  state->speed = buffer->speed;
  state->paused = buffer->paused;
  state->complete = buffer->complete;
  pthread_mutex_unlock(&buffer->lock);
}

int audio_buffer_is_finished(AudioBuffer *buffer) {
  pthread_mutex_lock(&buffer->lock);
  int finished =
      buffer->complete && buffer->position_frames >= buffer->total_frames;
  pthread_mutex_unlock(&buffer->lock);
  return finished;
}
