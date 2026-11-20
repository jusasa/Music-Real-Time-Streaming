#include "wav_file.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_u32le(const uint8_t *data) {
  return data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint16_t read_u16le(const uint8_t *data) {
  return (uint16_t)(data[0] | data[1] << 8);
}

int wav_file_open(WavFile *wav, const char *path) {
  memset(wav, 0, sizeof(*wav));
  wav->fp = fopen(path, "rb");
  if (!wav->fp) {
    perror(path);
    return -1;
  }

  uint8_t riff[12];
  if (fread(riff, 1, 12, wav->fp) != 12 || memcmp(riff, "RIFF", 4) ||
      memcmp(riff + 8, "WAVE", 4)) {
    fprintf(stderr, "[server] WAV 파일이 아닙니다: %s\n", path);
    return -1;
  }

  int got_fmt = 0;
  uint8_t chunk[8];
  while (fread(chunk, 1, 8, wav->fp) == 8) {
    uint32_t size = read_u32le(chunk + 4);
    if (!memcmp(chunk, "fmt ", 4)) {
      uint8_t format_chunk[16];
      if (size < 16 || fread(format_chunk, 1, 16, wav->fp) != 16)
        return -1;
      uint16_t format = read_u16le(format_chunk);
      wav->meta.channels = read_u16le(format_chunk + 2);
      wav->meta.sample_rate = read_u32le(format_chunk + 4);
      wav->meta.bits_per_sample = read_u16le(format_chunk + 14);
      if (format != 1 && format != 0xFFFE) {
        fprintf(stderr,
                "[server] 압축된 WAV 는 지원하지 않습니다 (PCM 만 가능)\n");
        return -1;
      }
      fseek(wav->fp, size - 16 + (size & 1), SEEK_CUR);
      got_fmt = 1;
    } else if (!memcmp(chunk, "data", 4)) {
      wav->data_offset = ftell(wav->fp);
      wav->data_size = size;
      break;
    } else {
      fseek(wav->fp, size + (size & 1), SEEK_CUR);
    }
  }

  if (!got_fmt || wav->data_offset == 0) {
    fprintf(stderr, "[server] fmt/data 청크를 찾지 못했습니다\n");
    return -1;
  }

  uint32_t frame_bytes =
      wav->meta.channels * (wav->meta.bits_per_sample / 8);
  if (frame_bytes == 0) {
    fprintf(stderr, "[server] 잘못된 WAV 오디오 포맷입니다\n");
    return -1;
  }
  wav->meta.total_frames = wav->data_size / frame_bytes;
  return 0;
}
