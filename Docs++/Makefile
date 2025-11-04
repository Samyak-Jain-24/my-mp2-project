CC=gcc
CFLAGS=-Wall -Wextra -O2 -Iinclude -pthread
LDFLAGS=-pthread

BIN_DIR=bin

CLIENT_SRCS=src/client/client.c src/client/client_ops.c src/common/network.c
CLIENT_OBJS=$(CLIENT_SRCS:.c=.o)

NS_SRCS=src/nameserver/nameserver.c src/nameserver/ns_handler.c src/nameserver/file_registry.c src/common/network.c
NS_OBJS=$(NS_SRCS:.c=.o)

SS_SRCS=src/storageserver/storageserver.c src/storageserver/ss_handler.c src/storageserver/file_ops.c src/storageserver/locking.c src/storageserver/undo.c src/common/network.c
SS_OBJS=$(SS_SRCS:.c=.o)

all: $(BIN_DIR)/client $(BIN_DIR)/nameserver $(BIN_DIR)/storageserver

$(BIN_DIR)/client: $(CLIENT_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/nameserver: $(NS_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/storageserver: $(SS_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(CLIENT_OBJS) $(NS_OBJS) $(SS_OBJS) $(BIN_DIR)/client $(BIN_DIR)/nameserver $(BIN_DIR)/storageserver

.PHONY: all clean
