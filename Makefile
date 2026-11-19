CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
TARGET  := mini-redis
ECHO    := echo-server
SRCS    := src/server.c src/resp.c src/dict.c
OBJS    := $(SRCS:.c=.o)

.PHONY: all run test test-m0 clean

all: $(TARGET) $(ECHO)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(ECHO): src/echo_server.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET) 7777

test: $(TARGET)
	python3 tests/m1_resp_test.py --bin ./$(TARGET)

test-m0: $(ECHO)
	python3 tests/m0_echo_test.py --bin ./$(ECHO)

clean:
	rm -f $(TARGET) $(ECHO) $(OBJS)