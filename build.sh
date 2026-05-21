#!/usr/bin/env bash

set -e
set -o pipefail

cc darkloard.c -lX11 -o darkloard
