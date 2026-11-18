CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
TARGET  := echo-server

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): src/echo_server.c
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET) 7777 et

test: $(TARGET)
	python3 tests/m0_echo_test.py --bin ./$(TARGET)

clean:
	rm -f $(TARGET)