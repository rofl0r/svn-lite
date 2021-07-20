PROG= svn
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

install: $(PROG) svn2git.sh
	install -Dm 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)
	install -Dm 755 svn2git.sh $(DESTDIR)$(PREFIX)/bin/svn2git

.PHONY: all clean install
