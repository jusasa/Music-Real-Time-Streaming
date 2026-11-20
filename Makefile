CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lpthread
PORTAUDIO_LIBS = -lportaudio

SERVER_BIN = bin/server
CLIENT_BIN = bin/client
TEST_BIN = bin/audio_buffer_test

SERVER_SRCS = $(wildcard server/*.c) $(wildcard server/lib/*.c)
CLIENT_SRCS = $(wildcard client/*.c) $(wildcard client/lib/*.c)

all: $(SERVER_BIN) $(CLIENT_BIN)

# 출력 디렉터리 생성
bin:
	mkdir -p bin

# 서버 빌드 (pthread 링크)
$(SERVER_BIN): $(SERVER_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 클라이언트 빌드 (PortAudio + pthread 링크)
$(CLIENT_BIN): $(CLIENT_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(PORTAUDIO_LIBS)

server: $(SERVER_BIN)
client: $(CLIENT_BIN)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): tests/audio_buffer_test.c client/lib/audio_buffer.c client/lib/audio_buffer.h | bin
	$(CC) $(CFLAGS) -Iclient/lib -o $@ tests/audio_buffer_test.c client/lib/audio_buffer.c $(LDFLAGS)

clean:
	rm -rf bin

.PHONY: all server client test clean