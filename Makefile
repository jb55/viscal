
BIN ?= viscal

all: $(BIN)

DEPS=libical gtk+-3.0

CFLAGS=-Wall \
       -Wextra \
       -O0 \
       -Wno-unused-parameter \
       -Werror=int-conversion \
			 -std=c99 \
			 -g \
			 -lm \
       `pkg-config --cflags --libs $(DEPS)`

tags: TAGS

$(BIN): calendar.c Makefile
	$(CC) $(CFLAGS) -o $@ $<

TAGS:
	./scripts/mktags libical > $@

clean:
	rm -f $(BIN)

.PHONY: TAGS clean tags
