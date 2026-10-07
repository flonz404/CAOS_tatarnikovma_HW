#!/bin/sh

gcc -std=c11 -Wall -Wextra hotel.c -o hotel || exit 1

mkdir -p results || exit 1

./hotel < test_cutoff.txt > results/cutoff.out || exit 1
cp hotel.log results/cutoff.log || exit 1

./hotel < test_empty.txt > results/empty.out || exit 1
cp hotel.log results/empty.log || exit 1

./hotel < test_rooms.txt > results/rooms.out || exit 1
cp hotel.log results/rooms.log || exit 1
