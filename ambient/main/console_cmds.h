#ifndef AMBIENT_CONSOLE_CMDS_H
#define AMBIENT_CONSOLE_CMDS_H

// Serial console on the USB port (ESP-IDF console component, its own task).
// Bench commands: help, status, info, config, push, quiet, reboot.
// Returns 0 when the console is running; -1 if it couldn't start (logged;
// the logger carries on without it).
int console_start(void);

#endif
