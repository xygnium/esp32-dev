#!/bin/bash

export IDF_TOOLS_PATH=$HOME/dev/esp32/tools
source $HOME/dev/esp32/esp-idf/export.sh
# ./monitor.sh --no-reset   attaches without resetting the board
idf.py -p /dev/ttyUSB0 monitor "$@"
