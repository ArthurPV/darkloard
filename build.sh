#!/usr/bin/env bash

set -e
set -o pipefail

cp --update=none config.h.def config.h
cc -std=c99 -O3 -Wall -Werror darkloard.c -I/usr/include/freetype2 -lX11 -lXft -lfontconfig -o darkloard
