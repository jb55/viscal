
BIN ?= viscal

all: $(BIN)

PREFIX ?= /usr
DEPS=libical gtk+-3.0

CFLAGS=-Wall \
       -Wextra \
       -O2 \
       -Wno-unused-parameter \
       -Werror=int-conversion \
			 -std=c99 \
			 -ggdb \
			 -lm \
       `pkg-config --cflags --libs $(DEPS)`

install: $(BIN)
	mkdir -p $(PREFIX)/bin
	cp $(BIN) $(PREFIX)/bin

tags: TAGS

$(BIN): viscal.c Makefile
	$(CC) $(CFLAGS) -o $@ $<

tags:
	ctags *.{c,h} > $@

check: $(BIN)
	echo "write tests!"

clean:
	rm -f $(BIN)

.PHONY: TAGS clean tags
