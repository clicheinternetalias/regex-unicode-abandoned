
CC=gcc -g
LD=gcc

CFLAGS=-Wall -Wextra -std=gnu99 -Wformat -Wshadow -Wconversion \
	-Wredundant-decls -Wpointer-arith -Wcast-align -Werror -pedantic -O2

LDFLAGS=`icu-config --ldflags --ldflags-icuio` -lgc

HFILES=

test: re
	./re < tests.txt
#	valgrind ./re < tests.txt
#	gprof ./re < tests.txt

re: regex.o bml.o icu-payne.o
	$(LD) -o re regex.o bml.o icu-payne.o $(LDFLAGS)

regex.o: regex.c icu-payne.h
	$(CC) $(CFLAGS) -c regex.c

bml.o: bml.c bml.h
	$(CC) $(CFLAGS) -c bml.c

icu-payne.o: icu-payne.c icu-payne.h braces.c.inc
	$(CC) $(CFLAGS) -c icu-payne.c

braces.c.inc: makebraces
	./makebraces > braces.c.inc

makebraces: makebraces.c
	$(LD) $(CFLAGS) -o makebraces makebraces.c

clean:
	rm -f *.o re makebraces braces.c.inc
