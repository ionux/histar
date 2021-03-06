#!/bin/sh -ex
for ARCH in amd64 i386 sparc um llvm arm; do
    make EMBEDBINS_DEFAULT=empty K_ARCH=$ARCH OBJSUFFIX=.kbuild
done

for ARCH in amd64 i386; do
    make K_ARCH=$ARCH OBJSUFFIX=.full all obj.full.$ARCH/boot/boot.iso
done

