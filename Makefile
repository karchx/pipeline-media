.PHONY: build

build:
	g++ -o pipemedia main.cc -lwebpdemux -lwebp -lavformat -lavcodec -lswscale -lavutil -lx264 -lpthread -lswresample -lm

.PHONY: run

run: build
	./pipemedia

.PHONY: rungdb
rungdb: build
	gdb ./pipemedia

.PHONY: runctx

runctx:
	g++ -g -fsanitize=address -o pipemedia main.cc -lwebpdemux -lwebp -lavformat -lavcodec -lswscale -lavutil -lx264 -lpthread -lswresample -lm
