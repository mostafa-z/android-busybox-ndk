#!/bin/sh

#We need full compiler including the *.o files! as they used to finish the build for static busybox.
#If compiler is in GIT directory then all *.o files are deleted in make clen command.
CC_STRIP=Android-ToolChain/bin/arm-eabi-strip
$CC_STRIP --strip-unneeded busybox
$CC_STRIP --strip-debug busybox
make -j16
