#!/bin/sh
echo    "******************************"
echo    "*     Make AArch64 Kernel    *"
echo    "******************************"
export ARCH=arm64
make rk3399-firefly-linux.img -j4
