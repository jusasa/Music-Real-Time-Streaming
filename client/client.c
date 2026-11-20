/*
 * client.c - TCP 실시간 음악 스트리밍 클라이언트 (진입점)
 *
 * 서버로부터 바이너리 패킷(PacketHeader + Payload)을 수신하여
 * PCM 오디오를 PortAudio로 저지연 재생한다.
 *
 * 실제 기능은 client/lib/ 의 모듈로 분리되어 있다.
 *   - net_util       : 소켓 연결 / recv_all
 *   - audio_buffer   : 수신 데이터 캐시 / 재생 위치 및 속도
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
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#include "lib/audio_buffer.h"
#include "lib/audio_player.h"
#include "lib/net_util.h"
#include "lib/packet_handler.h"

/* 전역 실행 상태 */
static volatile sig_atomic_t g_running = 1;
static struct termios g_original_terminal;
static int g_terminal_active = 0;

#define PROGRESS_WIDTH 40
#define SEEK_STEP_SECONDS 5

static void on_sigint(int sig) {
  (void)sig;
  g_running = 0;
}

static int enable_terminal_input(void) {
  if (!isatty(STDIN_FILENO))
    return 0;
  if (tcgetattr(STDIN_FILENO, &g_original_terminal) != 0)
    return -1;

  struct termios raw = g_original_terminal;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    return -1;

  g_terminal_active = 1;
  return 1;
}

static void restore_terminal(void) {
  if (g_terminal_active) {
    if (tcsetattr(STDIN_FILENO, TCSANOW, &g_original_terminal) != 0)
      perror("tcsetattr");
    g_terminal_active = 0;
  }
}

static int read_key(void) {
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(STDIN_FILENO, &read_fds);
  struct timeval timeout = {0, 0};
  if (select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout) <= 0)
    return -1;

  unsigned char key;
  if (read(STDIN_FILENO, &key, 1) != 1)
    return -1;
  if (key != '\033')
    return key;

  FD_ZERO(&read_fds);
  FD_SET(STDIN_FILENO, &read_fds);
  timeout.tv_usec = 10000;
  if (select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout) <= 0)
    return -1;
  unsigned char bracket;
  if (read(STDIN_FILENO, &bracket, 1) != 1 || bracket != '[')
    return -1;
  if (read(STDIN_FILENO, &key, 1) != 1)
    return -1;
  return key;
}

static void format_time(char *out, size_t size, double frames,
                        uint32_t sample_rate) {
  uint64_t seconds = sample_rate ? (uint64_t)(frames / sample_rate) : 0;
  uint64_t hours = seconds / 3600;
  uint64_t minutes = (seconds / 60) % 60;
  seconds %= 60;
  if (hours > 0)
    snprintf(out, size, "%02llu:%02llu:%02llu",
             (unsigned long long)hours, (unsigned long long)minutes,
             (unsigned long long)seconds);
  else
    snprintf(out, size, "%02llu:%02llu", (unsigned long long)minutes,
             (unsigned long long)seconds);
}

static void render_progress(AudioBuffer *buffer, uint32_t sample_rate,
                            int terminal_mode) {
  AudioBufferState state;
  audio_buffer_snapshot(buffer, &state);

  double position = state.position_frames;
  if (position > state.buffered_frames)
    position = state.buffered_frames;
  if (position > state.total_frames)
    position = state.total_frames;

  size_t position_col = 0;
  size_t buffered_col = 0;
  if (state.total_frames > 0) {
    position_col =
        (size_t)((long double)position / state.total_frames * PROGRESS_WIDTH);
    buffered_col = (size_t)((long double)state.buffered_frames /
                            state.total_frames * PROGRESS_WIDTH);
    if (position_col >= PROGRESS_WIDTH)
      position_col = PROGRESS_WIDTH - 1;
    if (buffered_col > PROGRESS_WIDTH)
      buffered_col = PROGRESS_WIDTH;
  }

  char position_text[24];
  char duration_text[24];
  format_time(position_text, sizeof(position_text), position, sample_rate);
  format_time(duration_text, sizeof(duration_text), state.total_frames,
              sample_rate);

  char bar[PROGRESS_WIDTH + 1];
  for (size_t i = 0; i < PROGRESS_WIDTH; i++) {
    if (i == position_col)
      bar[i] = '>';
    else if (i < position_col)
      bar[i] = '=';
    else if (i < buffered_col)
      bar[i] = '-';
    else
      bar[i] = ' ';
  }
  bar[PROGRESS_WIDTH] = '\0';

  unsigned int buffered_percent =
      state.total_frames > 0
          ? (unsigned int)((long double)state.buffered_frames * 100 /
                           state.total_frames)
          : 0;
  const char *status = state.paused
                           ? "일시정지"
                           : (!state.complete &&
                                      position >= state.buffered_frames
                                  ? "버퍼링"
                                  : "재생");
  printf("\r\033[K[%s] %s / %s | 버퍼 %u%% | %.2fx | %s",
         bar, position_text, duration_text, buffered_percent, state.speed,
         status);
  if (!terminal_mode)
    putchar('\n');
  fflush(stdout);
}

static void handle_key(int key, AudioBuffer *buffer, uint32_t sample_rate) {
  int64_t seek_frames = (int64_t)sample_rate * SEEK_STEP_SECONDS;
  switch (key) {
  case 'q':
  case 'Q':
    g_running = 0;
    break;
  case ' ':
    audio_buffer_toggle_paused(buffer);
    break;
  case '+':
  case '=':
    audio_buffer_adjust_speed(buffer, 1);
    break;
  case '-':
    audio_buffer_adjust_speed(buffer, -1);
    break;
  case 'D':
  case 'a':
  case 'A':
    audio_buffer_seek(buffer, -seek_frames);
    break;
  case 'C':
  case 'd':
    audio_buffer_seek(buffer, seek_frames);
    break;
  }
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

  /* 2) 수신된 PCM 캐시 초기화 */
  AudioBuffer buffer;
  if (audio_buffer_init(&buffer) != 0) {
    perror("audio_buffer_init");
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
      .buffer = &buffer,
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
    audio_buffer_destroy(&buffer);
    close(fd);
    return EXIT_FAILURE;
  }

  /* 4) 오디오 메타데이터 대기 */
  while (!meta_ready && running && g_running)
    usleep(10 * 1000);

  if (!meta_ready) {
    fprintf(stderr, "[client] 오디오 메타데이터를 받지 못했습니다.\n");
    running = 0;
    shutdown(fd, SHUT_RDWR);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&player_lock);
    audio_buffer_destroy(&buffer);
    close(fd);
    return EXIT_FAILURE;
  }

  /* 5) PortAudio 재생 시작 */
  AudioPlayer player;
  if (audio_player_start(&player, &buffer, &meta) != 0) {
    running = 0;
    shutdown(fd, SHUT_RDWR);
    pthread_join(tid, NULL);
    pthread_mutex_destroy(&player_lock);
    audio_buffer_destroy(&buffer);
    close(fd);
    return EXIT_FAILURE;
  }
  pthread_mutex_lock(&player_lock);
  ctx.player = &player;
  audio_player_set_paused(&player, ctx.paused);
  pthread_mutex_unlock(&player_lock);

  int terminal_mode = enable_terminal_input();
  if (terminal_mode < 0)
    perror("terminal mode");
  if (terminal_mode > 0)
    printf("[client] ←/→ 탐색(5초), +/- 배속, Space 일시정지, q 종료\n");
  else
    printf("[client] 키보드 조작은 터미널에서 실행할 때 사용할 수 있습니다.\n");

  /* 6) 재생 상태 및 키 입력 처리 */
  double last_render = 0;
  while (g_running && !audio_buffer_is_finished(&buffer)) {
    if (terminal_mode > 0) {
      int key = read_key();
      if (key >= 0)
        handle_key(key, &buffer, meta.sample_rate);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double now_seconds = now.tv_sec + now.tv_nsec / 1e9;
    if (now_seconds - last_render >= (terminal_mode > 0 ? 0.1 : 1.0)) {
      render_progress(&buffer, meta.sample_rate, terminal_mode > 0);
      last_render = now_seconds;
    }
    usleep(10 * 1000);
  }

  /* 7) 정리 */
  restore_terminal();
  printf("\n[client] 종료 중...\n");
  audio_player_stop(&player);

  shutdown(fd, SHUT_RDWR);
  close(fd);
  pthread_join(tid, NULL);
  pthread_mutex_destroy(&player_lock);
  audio_buffer_destroy(&buffer);

  printf("[client] 정상 종료되었습니다.\n");
  return EXIT_SUCCESS;
}
