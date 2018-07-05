SRC="main.c"

BASENAME="axisdiscovery"
VERSION=$(shell git describe --always --dirty)
MACHINE=$(shell $(CC) -dumpmachine)

CFLAGS=-Wall -DVERSION=\"$(VERSION)\"

PROG = $(BASENAME)-$(VERSION)-$(MACHINE)

all: $(PROG)

$(PROG): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@

$(SRC): Makefile

clean:
	rm -f $(PROG) *.o
