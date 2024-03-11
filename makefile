CFLAGS=-Wall -Wextra -O2 -I. -fPIC -flto

all: ccheck integer-provider.so

ccheck: ccheck.c interface.h
	cc $(CFLAGS) -rdynamic $< -o $@

integer-provider.so: integer-provider.c interface.h
	cc $(CFLAGS) -shared $< -o $@
