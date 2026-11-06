#!/bin/bash
# Test build with fault injection (`test fault ...`), in its own build
#   directory so the normal build (build.sh) never picks it up.

export IDF_TOOLS_PATH=$HOME/dev/esp32/tools
source $HOME/dev/esp32/esp-idf/export.sh
idf.py -B build-test -DAMBIENT_TEST_FAULTS=1 build
