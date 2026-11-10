
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#ifndef MSG_NOSIGNAL /* macOS 에는 없음 → SIGPIPE 무시로 대체 */
#define MSG_NOSIGNAL 0
#endif

/* 제어 명령 (PKT_CONTROL payload 1바이트)
 * ⚠ client/lib/control.h 의 값과 반드시 같아야 한다. 다르면 여기를 고칠 것. */
#ifndef CTRL_PLAY
#define CTRL_PLAY  1
#define CTRL_PAUSE 2
#define CTRL_STOP  3
#endif

#define MAX_AUDIO_CHUNK 4096 /* PCM 데이터 패킷 하나의 최대 크기 */
#define MAX_CLIENTS     16
#define SEND_TIMEOUT_S  2   /* 이 시간 동안 못 보내면 느린 클라이언트로 보고 끊음 */
#define PREBUFFER_MS    300 /* 시작 시 클라이언트 버퍼를 미리 채워 둘 양 */

/* ------------------------------------------------------------------ */
/* 전역 상태                                                            */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;
static int g_listen_fd = -1;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER; /* 일시정지 해제 알림 */
static int      g_clients[MAX_CLIENTS];
static int      g_client_count = 0;
static int      g_paused = 0;

/* WAV 정보 */
typedef struct
{
    FILE    *fp;
    long     data_offset; /* PCM 데이터 시작 위치 */
    uint32_t data_size;   /* PCM 데이터 크기     */
    AudioMetaPayload meta; /* 호스트 바이트 순서 */
} WavFile;

static WavFile g_wav;

static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_listen_fd >= 0)
        shutdown(g_listen_fd, SHUT_RDWR); /* accept() 깨우기 */
}

/* ------------------------------------------------------------------ */
/* 시간 유틸                                                            */
/* ------------------------------------------------------------------ */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_until(double deadline)
{
    double remain = deadline - now_sec();
    if (remain > 0)
        usleep((useconds_t)(remain * 1e6));
}

/* ------------------------------------------------------------------ */
/* WAV 파싱 (RIFF 청크를 돌면서 fmt / data 를 찾는다)                   */
/* ------------------------------------------------------------------ */

static uint32_t rd_u32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_open(WavFile *w, const char *path)
{
    memset(w, 0, sizeof(*w));
    w->fp = fopen(path, "rb");
    if (!w->fp)
    {
        perror(path);
        return -1;
    }

    uint8_t riff[12];
    if (fread(riff, 1, 12, w->fp) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4))
    {
        fprintf(stderr, "[server] WAV 파일이 아닙니다: %s\n", path);
        return -1;
    }

    int got_fmt = 0;
    uint8_t ch[8];
    while (fread(ch, 1, 8, w->fp) == 8)
    {
        uint32_t size = rd_u32le(ch + 4);
        if (!memcmp(ch, "fmt ", 4))
        {
            uint8_t fmt[16];
            if (size < 16 || fread(fmt, 1, 16, w->fp) != 16)
                return -1;
            uint16_t format = rd_u16le(fmt);
            w->meta.channels        = rd_u16le(fmt + 2);
            w->meta.sample_rate     = rd_u32le(fmt + 4);
            w->meta.bits_per_sample = rd_u16le(fmt + 14);
            if (format != 1 /* PCM */ && format != 0xFFFE /* EXTENSIBLE */)
            {
                fprintf(stderr, "[server] 압축된 WAV 는 지원하지 않습니다 (PCM 만 가능)\n");
                return -1;
            }
            fseek(w->fp, size - 16 + (size & 1), SEEK_CUR);
            got_fmt = 1;
        }
        else if (!memcmp(ch, "data", 4))
        {
            w->data_offset = ftell(w->fp);
            w->data_size   = size;
            break;
        }
        else
        {
            fseek(w->fp, size + (size & 1), SEEK_CUR); /* LIST 등 건너뛰기 */
        }
    }

    if (!got_fmt || w->data_offset == 0)
    {
        fprintf(stderr, "[server] fmt/data 청크를 찾지 못했습니다\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 패킷 송신                                                            */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0)
    {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1; /* EAGAIN(타임아웃), EPIPE 등 */
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 헤더 + payload 를 하나의 버퍼로 만들어 한 번에 보낸다 */
static uint64_t epoch_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int send_packet(int fd, uint16_t type, const void *payload, uint32_t len)
{
    uint8_t buf[sizeof(PacketHeader) + MAX_AUDIO_CHUNK];
    if (len > MAX_AUDIO_CHUNK)
        return -1;

    PacketHeader h;
    h.magic        = PACKET_MAGIC;
    h.type         = type;
    h.payload_len  = len;
    h.timestamp_ms = epoch_ms(); /* 서버 기준 전송 시각 (Unix ms) */

    memcpy(buf, &h, sizeof(h));
    if (len > 0)
        memcpy(buf + sizeof(h), payload, len);
    return send_all(fd, buf, sizeof(h) + len);
}

/* 모든 클라이언트에게 전송. 실패한 클라이언트는 목록에서 제거.
 * 반드시 g_lock 을 잡은 상태에서 호출할 것. */
static void broadcast_locked(uint16_t type, const void *payload, uint32_t len)
{
    for (int i = 0; i < g_client_count;)
    {
        if (send_packet(g_clients[i], type, payload, len) != 0)
        {
            printf("[server] 클라이언트(fd=%d) 연결 끊김 → 제거\n", g_clients[i]);
            close(g_clients[i]);
            g_clients[i] = g_clients[--g_client_count]; /* 마지막 원소로 채움 */
            continue;
        }
        i++;
    }
}

static void broadcast_control(uint8_t cmd)
{
    pthread_mutex_lock(&g_lock);
    broadcast_locked(PKT_CONTROL, &cmd, 1);
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* 방송 스레드: WAV 를 실시간 속도로 읽어 전송                          */
/* ------------------------------------------------------------------ */

static void *broadcast_thread(void *arg)
{
    (void)arg;
    const AudioMetaPayload *m = &g_wav.meta;
    const uint32_t frame_bytes = m->channels * (m->bits_per_sample / 8);
    const double bytes_per_sec = (double)m->sample_rate * frame_bytes;

    /* 청크 크기를 프레임 단위로 맞춘다 (샘플이 쪼개지지 않도록) */
    const uint32_t chunk = (MAX_AUDIO_CHUNK / frame_bytes) * frame_bytes;
    const double chunk_sec = chunk / bytes_per_sec;

    uint8_t buf[MAX_AUDIO_CHUNK];
    uint32_t sent_total = 0;
    /* 첫 번째 클라이언트가 들어올 때까지 대기 (곡 앞부분을 놓치지 않도록) */
    pthread_mutex_lock(&g_lock);
    while (g_client_count == 0 && g_running)
        pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);

    double deadline = now_sec();
    double lead = PREBUFFER_MS / 1000.0; /* 처음엔 이만큼 앞서서 보냄 */

    if (!g_running)
        return NULL;
    printf("[server] 방송 시작: %u Hz, %u ch, %u bit, 청크 %u B (%.1f ms)\n",
           m->sample_rate, m->channels, m->bits_per_sample, chunk, chunk_sec * 1000);

    while (g_running && sent_total < g_wav.data_size)
    {
        /* 일시정지 상태면 대기 */
        pthread_mutex_lock(&g_lock);
        int was_paused = 0;
        while (g_paused && g_running)
        {
            was_paused = 1;
            pthread_cond_wait(&g_cond, &g_lock);
        }
        pthread_mutex_unlock(&g_lock);
        if (was_paused)
            deadline = now_sec(); /* 재개 시점부터 다시 시간 계산 */

        uint32_t want = g_wav.data_size - sent_total;
        if (want > chunk)
            want = chunk;
        size_t n = fread(buf, 1, want, g_wav.fp);
        if (n == 0)
            break;
        sent_total += (uint32_t)n;

        pthread_mutex_lock(&g_lock);
        broadcast_locked(PKT_AUDIO_DATA, buf, (uint32_t)n);
        pthread_mutex_unlock(&g_lock);

        /* 실시간 속도 유지: 보낸 양만큼 시간이 흐를 때까지 대기 */
        deadline += n / bytes_per_sec;
        if (lead > 0)
            lead -= n / bytes_per_sec; /* 프리버퍼 구간은 쉬지 않고 전송 */
        else
            sleep_until(deadline - PREBUFFER_MS / 1000.0);
    }

    if (g_running)
    {
        /* 클라이언트 버퍼에 남은 소리가 재생될 시간을 준 뒤 종료 알림 */
        sleep_until(deadline);
        /* protocol.h 에 종료 패킷이 없으므로 CTRL_STOP 으로 곡 끝을 알린다 */
        printf("[server] 곡 재생 완료 → STOP 전송\n");
        broadcast_control(CTRL_STOP);
    }

    g_running = 0;
    if (g_listen_fd >= 0)
        shutdown(g_listen_fd, SHUT_RDWR);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 콘솔 스레드: p(일시정지) / r(재개) / q(종료)                          */
/* ------------------------------------------------------------------ */

static void *console_thread(void *arg)
{
    (void)arg;
    char line[64];
    printf("[server] 명령: p=일시정지, r=재개, l=접속자 목록, q=종료\n");

    while (g_running)
    {
        /* stdin 에 입력이 있을 때만 읽음 (종료 시 블록되지 않도록) */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        struct timeval tv = {0, 200 * 1000};
        if (select(STDIN_FILENO + 1, &rf, NULL, NULL, &tv) <= 0)
            continue;
        if (!fgets(line, sizeof(line), stdin))
            break;

        switch (line[0])
        {
        case 'p':
            pthread_mutex_lock(&g_lock);
            g_paused = 1;
            pthread_mutex_unlock(&g_lock);
            broadcast_control(CTRL_PAUSE);
            printf("[server] ⏸ 일시정지\n");
            break;
        case 'r':
            pthread_mutex_lock(&g_lock);
            g_paused = 0;
            pthread_cond_broadcast(&g_cond);
            pthread_mutex_unlock(&g_lock);
            broadcast_control(CTRL_PLAY);
            printf("[server] ▶ 재개\n");
            break;
        case 'l':
            pthread_mutex_lock(&g_lock);
            printf("[server] 접속자 %d명\n", g_client_count);
            pthread_mutex_unlock(&g_lock);
            break;
        case 'q':
            broadcast_control(CTRL_STOP);
            on_sigint(0);
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* main: 리슨 소켓 + accept 루프                                        */
/* ------------------------------------------------------------------ */

static int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* 새 클라이언트 등록: 소켓 옵션 설정 → META 전송 → 목록 추가 */
static void add_client(int cfd, const struct sockaddr_in *addr)
{
    int yes = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)); /* 저지연 */
    struct timeval tv = {SEND_TIMEOUT_S, 0};
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)); /* 느린 클라이언트 감지 */
#ifdef SO_NOSIGPIPE
    setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

    pthread_mutex_lock(&g_lock);
    if (g_client_count >= MAX_CLIENTS)
    {
        pthread_mutex_unlock(&g_lock);
        printf("[server] %s 거부 (최대 %d명)\n", ip, MAX_CLIENTS);
        close(cfd);
        return;
    }

    /* 락을 잡은 채로 META 를 보내야 데이터 패킷보다 META 가 먼저 도착한다 */
    if (send_packet(cfd, PKT_AUDIO_INIT, &g_wav.meta, sizeof(g_wav.meta)) != 0)
    {
        pthread_mutex_unlock(&g_lock);
        close(cfd);
        return;
    }
    /* 일시정지 중에 들어왔다면 그 상태도 알려 준다 */
    if (g_paused)
    {
        uint8_t c = CTRL_PAUSE;
        send_packet(cfd, PKT_CONTROL, &c, 1);
    }

    g_clients[g_client_count++] = cfd;
    int count = g_client_count;
    pthread_cond_broadcast(&g_cond); /* 첫 접속을 기다리는 방송 스레드 깨우기 */
    pthread_mutex_unlock(&g_lock);

    printf("[server] %s:%d 접속 (fd=%d, 현재 %d명)\n", ip, ntohs(addr->sin_port), cfd, count);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "사용법: %s <파일.wav> [port]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int port = (argc > 2) ? atoi(argv[2]) : SERVER_PORT;

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    if (wav_open(&g_wav, argv[1]) != 0)
        return EXIT_FAILURE;
    if (g_wav.meta.bits_per_sample != 16)
        fprintf(stderr, "[server] 경고: %u bit WAV 입니다. 클라이언트가 16bit 만 지원하면 소리가 깨집니다.\n",
                g_wav.meta.bits_per_sample);

    g_listen_fd = create_listen_socket(port);
    if (g_listen_fd < 0)
    {
        perror("listen");
        return EXIT_FAILURE;
    }
    printf("[server] 포트 %d 에서 대기 중... (%s, %.1f초)\n", port, argv[1],
           g_wav.data_size / ((double)g_wav.meta.sample_rate * g_wav.meta.channels *
                              (g_wav.meta.bits_per_sample / 8)));

    pthread_t bt, ct;
    pthread_create(&bt, NULL, broadcast_thread, NULL);
    pthread_create(&ct, NULL, console_thread, NULL);

    while (g_running)
    {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(g_listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0)
        {
            if (errno == EINTR)
                continue;
            break; /* shutdown() 으로 깨어난 경우 */
        }
        add_client(cfd, &caddr);
    }

    /* 정리 */
    printf("\n[server] 종료 중...\n");
    g_running = 0;
    pthread_mutex_lock(&g_lock);
    pthread_cond_broadcast(&g_cond); /* 일시정지 대기 중인 방송 스레드 깨우기 */
    pthread_mutex_unlock(&g_lock);

    pthread_join(bt, NULL);
    pthread_join(ct, NULL);

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_client_count; i++)
    {
        shutdown(g_clients[i], SHUT_RDWR);
        close(g_clients[i]);
    }
    g_client_count = 0;
    pthread_mutex_unlock(&g_lock);

    close(g_listen_fd);
    fclose(g_wav.fp);
    printf("[server] 정상 종료되었습니다.\n");
    return EXIT_SUCCESS;
}
