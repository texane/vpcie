#!/usr/bin/env sh

if [ ! -e /dev/sbone ]; then
    major=`dmesg | grep '\[ sbone \] major: ' | tail -n1 | cut -d: -f2`
    minor='0';
    mknod /dev/sbone c $major $minor;
fi

./sbone $@
