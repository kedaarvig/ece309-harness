CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2

.PHONY: all test clean

all: harness

harness: harness.c
	$(CC) $(CFLAGS) harness.c -o harness

test: harness
	bash test.sh

clean:
	rm -f harness harness_asan
