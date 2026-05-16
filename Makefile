CC ?= gcc
CFLAGS ?= -O2 -Wall
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

bukuloops: jungledaw.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $< $(SDL_LIBS) -lm

clean:
	rm -f bukuloops

.PHONY: clean
