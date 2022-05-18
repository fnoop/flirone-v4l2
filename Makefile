SANITIZERS ?=   -fsanitize=address -fsanitize=undefined \
                -fsanitize=pointer-compare -fsanitize=pointer-subtract \
                -fsanitize=leak -fno-sanitize-recover=all \
                -fsanitize-address-use-after-scope \
                -fstack-protector-all \
                -fstack-protector-strong

INCPATH = -I/usr/include/libusb-1.0
CFLAGS = $(INCPATH) -pipe -O2 -Wall -D_REENTRANT -g $(SANITIZERS)

all: flirone

flirone.o: src/flirone.c src/plank.h
	$(CC) $(CFLAGS) -c $< -o $@

flirone: flirone.o
	$(CC) -o $@ $< $(SANITIZERS) -lusb-1.0 -lm -g

clean:
	rm -f flirone.o flirone
