/*
 * The lwIP socket calls the shared link (esp32/main) makes, on ARCS sockets.
 *
 * Sockets here are SAL descriptors (components/lisa_net), not lwIP ones, and
 * SAL's select() hands the descriptor sets to lwIP unconverted, so lwIP's own
 * calls cannot be used on them. These do the two things noise_control.cpp
 * needs:
 * - lwip_fcntl(F_SETFL, O_NONBLOCK) switches the esp_tls connection on that
 *   socket to non-blocking reads (the socket itself always is: plat_tls.c).
 * - lwip_select() waits until the sockets in readfds can be read (data or
 *   end of stream) or the timeout passes; writefds and exceptfds are not
 *   supported (upstream passes NULL).
 */
#pragma once

#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

int esp_compat_fcntl(int fd, int cmd, int val);
int esp_compat_select(int maxfdp1, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);

#ifdef __cplusplus
}
#endif

#define lwip_fcntl(fd, cmd, val) esp_compat_fcntl((fd), (cmd), (val))
#define lwip_select(n, r, w, e, t) esp_compat_select((n), (r), (w), (e), (t))
