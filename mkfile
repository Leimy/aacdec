</$objtype/mkfile
BIN=/$objtype/bin/audio

TARGET=aacdec

CC=pcc
CFLAGS=-Ilibfaad -Ilibfaad/include -D_POSIX_SOURCE -D_BSD_EXTENSION -DHAVE_STDINT_H

%.$O: %.c
	$CC $CFLAGS -c $stem.c

$O.%: %.$O libfaad/libfaad.a$O
	$CC -o $target $prereq

libfaad/libfaad.a$O:
	cd libfaad && mk

all:V: $O.$TARGET

clean:V:
	rm -f *.[$OS] [$OS].$TARGET
	cd libfaad && mk clean

nuke:V:
	rm -f *.[$OS] [$OS].$TARGET
	cd libfaad && mk nuke

install:V: $O.$TARGET
	mkdir -p $BIN
	cp $O.$TARGET $BIN/$TARGET

test:VQ:
	# nothing
