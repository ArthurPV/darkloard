#!/usr/bin/env bash

set -e
set -o pipefail

cc -O3 -Wall -Werror darkloard.c -I/usr/include/freetype2 -lX11 -lXft -lfontconfig -o darkloard
