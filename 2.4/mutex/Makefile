CC = gcc
CFLAGS = -Wall -pthread -D_GNU_SOURCE
LDFLAGS = -lpthread
TARGET = queue_spinlock_test
SRCS = main.c queue.c spinlock.c
OBJS = $(SRCS:.c=.o)
HEADERS = queue.h spinlock.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all run clean