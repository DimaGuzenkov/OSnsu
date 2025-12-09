CC = gcc
CFLAGS = -Wall -pthread -D_GNU_SOURCE
TARGET = queue_spinlock_test

all: $(TARGET)

queue_spinlock_test: main.o queue.o spinlock.o
	$(CC) $(CFLAGS) -o $@ main.o queue.o spinlock.o

main.o: main.c queue.h spinlock.h
	$(CC) $(CFLAGS) -c main.c

queue.o: queue.c queue.h spinlock.h
	$(CC) $(CFLAGS) -c queue.c

spinlock.o: spinlock.c spinlock.h
	$(CC) $(CFLAGS) -c spinlock.c

clean:
	rm -f $(TARGET) *.o

run: $(TARGET)
	./$(TARGET)