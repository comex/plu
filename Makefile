CC ?= gcc
plu: plu.c Makefile
	$(CC) -o plu plu.c -framework CoreFoundation
