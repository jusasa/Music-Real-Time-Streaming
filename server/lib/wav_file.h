#ifndef WAV_FILE_H
#define WAV_FILE_H

#include <stdio.h>

#include "protocol.h"

typedef struct {
  FILE *fp;
  long data_offset;
  uint32_t data_size;
  AudioMetaPayload meta;
} WavFile;

int wav_file_open(WavFile *wav, const char *path);

#endif
