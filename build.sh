#!/usr/bin/env bash

set -e
set -o pipefail

if [ ! -f config.h ]
then
	cp config.h.def config.h
fi

EXTRA_OPTIONS=""
OS="$(uname -s)"

case "$OS" in
	Linux)
		EXTRA_OPTIONS="-I/usr/include/freetype2"
		;;
	FreeBSD)
		EXTRA_OPTIONS="-I/usr/local/include -I/usr/local/include/freetype2 -L/usr/local/lib -lutil"
		;;
	NetBSD)
		EXTRA_OPTIONS="-I/usr/X11R7/include -I/usr/X11R7/include/freetype2 -L/usr/X11R7/lib -lutil"
		;;
	*)
		echo "This OS is not supported: $OS"
		exit 1
esac

cc -std=c99 -O3 -Wall -Werror darkloard.c $EXTRA_OPTIONS -lX11 -lXft -lfontconfig -o darkloard
