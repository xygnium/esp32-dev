#!/bin/bash

if [ $# -ne 0 ]; then
    echo "usage: $0   (takes no options)" >&2
    exit 1
fi
export IDF_TOOLS_PATH=$HOME/dev/esp32/tools
source $HOME/dev/esp32/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 monitor
