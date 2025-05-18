
CC	= gcc
CPP	= g++
CFLAGS	= -g -Wall
CPPFLAGS=

default: all

all:	em-admin

em-admin:	em-admin.c
	$(CC) $(CFLAGS) -o em-admin em-admin.c

clean:
	rm -f em-admin

