# LangOS Distributed File System Makefile

CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

# Targets
all: name_server storage_server client

name_server: name_server.o common.o
	$(CC) $(LDFLAGS) -o name_server name_server.o common.o

storage_server: storage_server.o common.o
	$(CC) $(LDFLAGS) -o storage_server storage_server.o common.o

client: client.o common.o
	$(CC) $(LDFLAGS) -o client client.o common.o

name_server.o: name_server.c common.h
	$(CC) $(CFLAGS) -c name_server.c

storage_server.o: storage_server.c common.h
	$(CC) $(CFLAGS) -c storage_server.c

client.o: client.c common.h
	$(CC) $(CFLAGS) -c client.c

common.o: common.c common.h
	$(CC) $(CFLAGS) -c common.c

clean:
	rm -f *.o name_server storage_server client
	rm -f *.log
	rm -f nm_data.dat

cleanall: clean
	rm -rf storage*/
	rm -f *.log *.dat

run_nm: name_server
	./name_server

run_ss1: storage_server
	mkdir -p storage1
	./storage_server 9001 9101 storage1/

run_ss2: storage_server
	mkdir -p storage2
	./storage_server 9002 9102 storage2/

run_client: client
	./client

.PHONY: all clean cleanall run_nm run_ss1 run_ss2 run_client
