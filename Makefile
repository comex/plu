CC ?= gcc
plu: plu.c Makefile
	$(CC) -o plu plu.c -std=gnu99 -framework CoreFoundation
