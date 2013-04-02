#!/usr/bin/env sh

if [ ! -e /dev/kdma0 ]; then
    major=`dmesg | grep '\[ kdma \] major: ' | tail -n1 | cut -d: -f2`
    minor='0';
    mknod /dev/kdma0 c $major $minor;
fi

./udma $@
