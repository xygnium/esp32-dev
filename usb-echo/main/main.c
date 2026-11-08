// USB echo. Collects what is typed in the monitor and, when Return is
//   pressed, sends the line back as "you typed: ..." on a new line. It reads
//   UART0, the serial line to the board's CP2102 USB bridge. Characters that
//   don't print (Backspace, Tab, ...) come back as <08>, <09> and so on. A LF
//   straight after a CR is part of the same Return. Nothing else is printed
//   after the ready line.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

#define PORT    UART_NUM_0
#define BAUD    115200
#define BUF     256
#define LINE_MAX_CHARS 200   // a longer line is sent back in pieces

void app_main(void)
{
    const uart_config_t cfg = {
        .baud_rate = BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(PORT, BUF * 2, BUF * 2, 0, NULL, 0);
    uart_param_config(PORT, &cfg);

    static const char ready[] = "usb-echo ready: type a line and press Return\r\n";
    uart_write_bytes(PORT, ready, sizeof ready - 1);

    static uint8_t buf[BUF];
    static char line[4 * LINE_MAX_CHARS + 1];
    static char out[sizeof line + 32];
    int len = 0, chars = 0;
    uint8_t prev = 0;
    for (;;) {
        int n = uart_read_bytes(PORT, buf, sizeof buf, pdMS_TO_TICKS(10));
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            bool end = c == '\r' || (c == '\n' && prev != '\r');
            bool skip = c == '\n' && prev == '\r';
            prev = c;
            if (skip) {
                continue;
            }
            if (!end) {
                if (c >= 0x20 && c < 0x7F) {
                    line[len++] = (char)c;
                } else {
                    len += snprintf(line + len, sizeof line - len, "<%02x>", c);
                }
                chars++;
            }
            if (end || chars == LINE_MAX_CHARS) {
                line[len] = '\0';
                int olen = snprintf(out, sizeof out, "\r\nyou typed: %s\r\n", line);
                uart_write_bytes(PORT, out, olen);
                len = chars = 0;
            }
        }
    }
}
