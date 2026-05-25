#!/usr/bin/env bash

set -e
set -o pipefail

cc -Wall darkloard.c -I/usr/include/freetype2 -lX11 -lXft -lfontconfig -o darkloard
