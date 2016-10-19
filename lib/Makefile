.PHONY: clean lib

all: lib test test2

lib: libws2811-pcm.a


ws2811-pcm.o: ws2811-pcm.c
	gcc -o ws2811-pcm.o -c -g -O2 -Wall -Werror ws2811-pcm.c -fPIC

rpihw.o: rpihw.c
	gcc -o rpihw.o -c -g -O2 -Wall -Werror rpihw.c -fPIC

pcm.o: pcm.c
	gcc -o pcm.o -c -g -O2 -Wall -Werror pcm.c -fPIC

dma.o: dma.c
	gcc -o dma.o -c -g -O2 -Wall -Werror dma.c -fPIC

mailbox.o: mailbox.c
	gcc -o mailbox.o -c -g -O2 -Wall -Werror mailbox.c -fPIC

libws2811-pcm.a: ws2811-pcm.o rpihw.o pcm.o dma.o mailbox.o
	ar rc libws2811-pcm.a ws2811-pcm.o rpihw.o pcm.o dma.o mailbox.o
	ranlib libws2811-pcm.a


main.o: main.c 
	gcc -o main.o -c -g -O2 -Wall -Werror main.c

test: main.o libws2811-pcm.a
	gcc -o test main.o libws2811-pcm.a

clean:
	-rm -f ws2811-pcm.o rpihw.o pcm.o dma.o mailbox.o libws2811-pcm.a main.o test
