/*
 * client.c - TCP 실시간 음악 스트리밍 클라이언트 (진입점)
 *
 * 서버로부터 바이너리 패킷(PacketHeader + Payload)을 수신하여
 * PCM 오디오를 PortAudio로 저지연 재생한다.
 *
 * 실제 기능은 client/lib/ 의 모듈로 분리되어 있다.
 *   - net_util       : 소켓 연결 / recv_all
 *   - ring_buffer    : 수신 스레드 <-> 오디오 콜백 링 버퍼
 *   - audio_player   : PortAudio 재생
 *   - packet_handler : 패킷 수신/처리 루프
 *   - control        : PKT_CONTROL 명령 상수
 *
 * 담당: Client (feature/client)
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

#include "lib/audio_player.h"
#include "lib/net_util.h"
#include "lib/packet_handler.h"
#include "lib/ring_buffer.h"

/* 재생 버퍼(링 버퍼) 크기: 1 MiB */
#define RING_CAPACITY (1024 * 1024)

/* 전역 실행 상태 */
static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) {
  (void)sig;
  g_running = 0;
}

int main(int argc, char *argv[]) {
  const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
  int port = (argc > 2) ? atoi(argv[2]) : SERVER_PORT;

  signal(SIGINT, on_sigint);
  signal(SIGPIPE, SIG_IGN);

  /* 1) TCP 연결 */
  int fd = connect_to_server(host, port);
  if (fd < 0) {
    fprintf(stderr, "[client] %s:%d 연결 실패\n", host, port);
    return EXIT_FAILURE;
  }
  printf("[client] %s:%d 에 연결되었습니다.\n", host, port);

  /* 2) 링 버퍼 초기화 */
  RingBuffer ring;
  if (ring_init(&ring, RING_CAPACITY) != 0) {
    perror("ring_init");
    close(fd);
    return EXIT_FAILURE;
  }

  /* 3) 수신 스레드 시작 */
  AudioMetaPayload meta;
  memset(&meta, 0, sizeof(meta));
  volatile int meta_ready = 0;
  int running = 1;
  pthread_mutex_t player_lock = PTHREAD_MUTEX_INITIALIZER;

  PacketContext ctx = {
      .fd = fd,
      .ring = &ring,
      .player = NULL, /* 오디오 스트림 시작 후 설정 */
      .player_lock = &player_lock,
      .paused = 0,
      .meta = &meta,
      .meta_ready = &meta_ready,
      .running = &running,
  };

  pthread_t tid;
  if (pthread_create(&tid, NULL, packet_recv_loop, &ctx) != 0) {
    perror("pthread_create");
    pthread_mutex_destroy(&player_lock);
    ring_destroy(&ring);
    close(fd);
    return EXIT_FAILURE;
  }

  /* 4) 오디오 메타데이터 대기 */
  while (!meta_ready && g_running)
    usleep(10 * 1000);

  if (!meta_ready) {
    fprintf(stderr, "[client] 오디오 메타데이터를 받지 못했습니다.\n");
    running = 0;
    ring_set_eof(&ring);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&player_lock);
    ring_destroy(&ring);
    close(fd);
    return EXIT_FAILURE;
  }

  /* 5) PortAudio 재생 시작 */
  AudioPlayer player;
  if (audio_player_start(&player, &ring, &meta) != 0) {
    running = 0;
    ring_set_eof(&ring);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&player_lock);
    ring_destroy(&ring);
    close(fd);
    return EXIT_FAILURE;
  }
  pthread_mutex_lock(&player_lock);
  ctx.player = &player;
  audio_player_set_paused(&player, ctx.paused);
  pthread_mutex_unlock(&player_lock);

  printf("[client] 🎶 스트리밍 재생을 시작합니다. (Ctrl+C 종료)\n");

  /* 6) 종료 대기 */
  while (g_running && running)
    usleep(100 * 1000);

  /* 7) 정리 */
  printf("\n[client] 종료 중...\n");
  audio_player_stop(&player);

  shutdown(fd, SHUT_RDWR);
  close(fd);
  pthread_join(tid, NULL);
  pthread_mutex_destroy(&player_lock);
  ring_destroy(&ring);

  printf("[client] 정상 종료되었습니다.\n");
  return EXIT_SUCCESS;
}
