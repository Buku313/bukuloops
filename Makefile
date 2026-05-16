CC ?= gcc
CFLAGS ?= -O2 -Wall
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)

bukuloops: jungledaw.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $< $(SDL_LIBS) -lm

# Emscripten web build
web: jungledaw.c shell.html
	emcc $< -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH=1 -s TOTAL_MEMORY=67108864 \
		-O2 --shell-file shell.html \
		--preload-file samples@/samples \
		-o web/index.html
	@echo "Web build ready in web/"

clean:
	rm -f bukuloops
	rm -rf web/

.PHONY: clean web
