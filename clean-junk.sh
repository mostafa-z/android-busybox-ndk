#!/bin/bash

for i in `find . -type f \( -iname \*.rej \
                                -o -iname \*.orig \
                                -o -iname \*.bkp \
                                -o -iname \*.ko \
				-o -iname \*.o \
                                -o -iname \*.c.BACKUP.[0-9]*.c \
                                -o -iname \*.c.BASE.[0-9]*.c \
                                -o -iname \*.c.LOCAL.[0-9]*.c \
                                -o -iname \*.c.REMOTE.[0-9]*.c \
                                -o -iname \*.org \)`; do
        rm -vf $i;
done;

git checkout Android-ToolChain/

if [ -f busybox ]; then
	rm busybox busybox_unstripped busybox_unstripped.map busybox_unstripped.out
fi;
