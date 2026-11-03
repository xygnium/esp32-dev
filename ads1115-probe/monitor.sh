#!/bin/bash
# Resets the board at open, so the start-up lines always print.
# The console is also saved to build/monitor.log (overwritten each run).

export IDF_TOOLS_PATH=$HOME/dev/esp32/tools
source $HOME/dev/esp32/esp-idf/export.sh
script -q -f -c "idf.py -p /dev/ttyUSB0 monitor" build/monitor.log
