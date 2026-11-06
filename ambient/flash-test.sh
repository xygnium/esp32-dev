#!/bin/bash
# Flash the test build (build-test.sh). Reflash with flash.sh for production.

export IDF_TOOLS_PATH=$HOME/dev/esp32/tools
source $HOME/dev/esp32/esp-idf/export.sh
idf.py -B build-test -p /dev/ttyUSB0 flash
