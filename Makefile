
CC     = gcc
CFLAGS = -Wall -std=c99 -O3
EXE    = image_tagger
OBJ    = server.o

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(CFLAGS) -o $(EXE) $(OBJ)

server.o: server.c

.PHONY: clean cleanly all CLEAN

clean:
	rm -f $(OBJ)
CLEAN: clean
	rm -f $(EXE)
cleanly: all clean