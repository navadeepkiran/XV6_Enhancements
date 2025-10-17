# Compiler and flags
CC=gcc
# Add -Werror back in once you are confident your code has no warnings
CFLAGS=-Wall -g 
LIBS=-lpcap

# Target executable name
TARGET=cshark

# Source files
SRCS=main.c sniffer.c utils.c
# Object files derived from source files
OBJS=$(SRCS:.c=.o)

# Default target
all: $(TARGET)

# Link object files to create the executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)

# Compile source files into object files
main.o: main.c sniffer.h utils.h
	$(CC) $(CFLAGS) -c main.c

sniffer.o: sniffer.c sniffer.h utils.h
	$(CC) $(CFLAGS) -c sniffer.c

utils.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c

# Clean up build files
clean:
	rm -f $(OBJS) $(TARGET)