#!/bin/sh

#We need full compiler including the *.o files! as they used to finish the build for static busybox.
#If compiler is in GIT directory then all *.o files are deleted in make clen command.
#make -j 16 ARCH=arm CROSS_COMPILE=/android-kernel/LG/LINARO_COMPILER/bin/arm-LG-linux-gnueabi-
#CC_STRIP=/android-kernel/LG/LINARO_COMPILER/bin/arm-LG-linux-gnueabi-strip
#$CC_STRIP --strip-unneeded busybox
#$CC_STRIP --strip-debug busybox

# use NDK 10d to compile. see .config for path.
make -j16

