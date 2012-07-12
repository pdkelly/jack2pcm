TARGET = jack2pcm
all: $(TARGET)

CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -s

INCLUDES = 
LIBS = -ljack
DEPS = 

OBJS = jack2pcm.o

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	
clean:
	rm $(OBJS) $(TARGET)
