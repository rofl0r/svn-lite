PROG= svnup
SRCS= svnup.c
OBJS= svnup.o

LDADD= -lssl -lcrypto

PREFIX=/usr/local

-include config.mak

all: $(PROG)

svnup.o: CPPFLAGS += -I.

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDADD)

clean:
	rm -f $(PROG) $(OBJS)

install:
	install -Dm 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)

.PHONY: all clean install
