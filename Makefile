PROG="axisdiscovery"
SRC="main.c"

VERSION=$(shell git describe --always --dirty)
CFLAGS=-Wall -DVERSION=\"$(VERSION)\"

all: $(PROG)

$(PROG): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@

$(SRC): Makefile

clean:
	rm -f $(PROG) *.o
