CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -fopenmp -pthread -Iinclude -Isrc
LIBS    = -lcjson -lncurses -lm

SERVER_SRCS = src/server_main.c src/game_logic.c src/game_state.c \
              src/net_handler.c src/pb_replication.c src/heartbeat.c
CLIENT_SRCS = src/client_main.c src/net_handler.c src/ui_cli.c

all: server client

server: $(SERVER_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

client: $(CLIENT_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f server client *.o
	rm -f $(FIFO_NET_TO_GAME) $(FIFO_GAME_TO_NET) 2>/dev/null || true

.PHONY: all clean
