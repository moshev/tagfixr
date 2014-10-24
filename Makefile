CC=gcc
CFLAGS=-ggdb -Og -std=c11 -Wall -Werror
LDFLAGS=`pkg-config --libs id3tag`

tagfixr: src/main.o
	$(CC) -o tagfixr $(LDFLAGS) src/main.o

clean:
	rm -rf src/*.o
	rm -f tagfixr

