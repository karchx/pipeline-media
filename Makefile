.PHONY: build

build:
	g++ -o pipemedia main.cc -lwebpdemux -lwebp -lavformat -lavcodec -lswscale -lavutil -lx264 -lpthread -lm

.PHONY: run

run: build
	./pipemedia
