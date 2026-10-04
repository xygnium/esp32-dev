#ifndef ESP32_COMMON_WIFI_H
#define ESP32_COMMON_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Station-mode WiFi plus a UDP client. The device never listens: it sends a
// datagram and waits for one reply (the logger pushes; the collector answers).

// Start WiFi in station mode and connect, blocking until an IP address is
// assigned or timeout_ms passes. Returns 0 when connected; -1 on timeout,
// with WiFi still trying in the background (poll wifi_is_connected()); -2 if
// WiFi setup itself failed (step and reason logged), in which case WiFi is not
// running and the caller should carry on without it. Never restarts the
// board. After any later disconnect it reconnects on its own, retrying once a
// second.
// Each connect scans every channel and joins the strongest access point
// broadcasting the SSID (e.g. router vs. extender), not the first to answer.
// Once connected it stays on that access point until the link drops.
// NVS (the ESP32's flash settings area) must be initialised first; WiFi
// keeps its radio calibration data there.
int wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

// Switch to another network (or fix a password): saves the new SSID and
// password in the WiFi driver, drops the current link, and reconnects with
// them. Returns 0, or -1 if WiFi isn't running.
int wifi_set_credentials(const char *ssid, const char *pass);

bool wifi_is_connected(void);

// Times an IP address was regained after the first connect.
uint32_t wifi_reconnect_count(void);

// Signal strength of the current access point in dBm. Returns 0 on success.
int wifi_rssi(int *rssi);

// Dotted-quad IP address of this device, or "0.0.0.0" when not connected.
void wifi_ip_str(char *buf, size_t size);

// Station MAC address as "aa:bb:cc:dd:ee:ff".
void wifi_mac_str(char *buf, size_t size);

// MAC of the access point currently joined (all zeros when not connected).
void wifi_bssid_str(char *buf, size_t size);

// Send msg to host:port (host is a dotted-quad IP) and wait up to timeout_ms
// for one reply. The reply is copied into reply (at most reply_size - 1
// bytes, NUL-terminated) and its length stored in *reply_len.
// Returns 0 when a reply arrived, -1 on a send/socket error, -2 on timeout.
int wifi_udp_exchange(const char *host, uint16_t port,
                      const void *msg, size_t msg_len,
                      char *reply, size_t reply_size, size_t *reply_len,
                      uint32_t timeout_ms);

#endif
