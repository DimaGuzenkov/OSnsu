CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -g
LDFLAGS=

SOURCES=uthread.c main.c
OBJECTS=$(SOURCES:.c=.o)
TARGET=uthread_example

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

run: $(TARGET)
	./$(TARGET)