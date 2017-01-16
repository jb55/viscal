
BIN ?= calendar

all: $(BIN)

DEPS=libical gtk+-3.0

CFLAGS=-Wall \
       -Wextra \
       -Wno-unused-parameter \
       -Werror=int-conversion \
			 -std=c99 \
			 -g \
       `pkg-config --cflags --libs $(DEPS)`

$(BIN): $(BIN).c Makefile
	$(CC) $(CFLAGS) -o $@ $<

TAGS:
	./scripts/mktags $(DEPS) > $@

clean:
	rm -f $(BIN)

.PHONY: TAGS clean
