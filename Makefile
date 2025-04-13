CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGET = udp_to_tcp
SOURCES = udp_to_tcp.c
OBJECTS = udp_to_tcp.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)