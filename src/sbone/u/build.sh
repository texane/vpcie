#!/usr/bin/env sh

gcc -Wall -O3 -static -I. -o sbone main.c sbone_dev.c -lpci -lz
