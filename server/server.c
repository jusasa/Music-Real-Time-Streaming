#include <stdio.h>
#include <stdlib.h>

#include "protocol.h"
#include "lib/server_runtime.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "사용법: %s <파일.wav> [port]\n", argv[0]);
    return EXIT_FAILURE;
  }

  int port = (argc > 2) ? atoi(argv[2]) : SERVER_PORT;
  return server_runtime_run(argv[1], port);
}
