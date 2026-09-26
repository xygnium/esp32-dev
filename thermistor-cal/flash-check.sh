#!/bin/bash
# Flash with full esptool output, then summarize: crystal reading, writes, hash checks.
# The log is kept in build/flash-check.log.

PORT=/dev/ttyUSB0
LOG=build/flash-check.log

if fuser $PORT >/dev/null 2>&1; then
    echo "$PORT is in use:"; fuser -v $PORT
    exit 1
fi

export IDF_TOOLS_PATH=$HOME/dev/esp32/tools
source $HOME/dev/esp32/esp-idf/export.sh >/dev/null
idf.py -p $PORT flash 2>&1 | tee $LOG
status=${PIPESTATUS[0]}

echo
echo "===== summary (exit $status) ====="
grep -E 'Crystal|WARNING|ERROR|Wrote|already in flash|Hash of data' $LOG
exit $status
