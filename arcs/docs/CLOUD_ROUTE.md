# Official cloud routing on ARCS

ARCS builds the official `esp32/components/impo_cloud_route` implementation
unchanged and uses its Kconfig. `CONFIG_IMPO_CHINA_RELAY` is enabled by default;
set it to `n` to connect directly. No separate gateway or host bridge is needed.

The November 24 SDK changes added mainland routing (`4e68720`), corrected it
to pass TLS through to Cloudflare (`0b83ffb`), and refresh/reuse VM credentials
according to their lifetime (`7d1f0ba`, `0e59456`). ARCS also compiles the shared
`noise_control.cpp` and `vm_api.c` directly, including that credential policy.
The ESP32 WAV data-chunk fix (`ed25ef0`) is in its separate sound implementation;
it does not replace the ARCS sound task.

## Peer and authenticated server

For a mainland connection the TCP peer is `gadget.itoy.cn:9443`, while SNI,
certificate hostname verification and the HTTP Host header use the shared
policy's `impo_cloud_route_tls_name()` (`gw.itoy.cn` in current upstream
`ad0e6b7`). Direct connections use `gadgets.impo.ai`. The relay copies encrypted bytes; TLS terminates at Impo's
Cloudflare endpoint. The `/where` lookup itself authenticates the relay hostname
and does not carry account credentials.

The ARCS ESP-IDF compatibility layer now carries `common_name` separately from
the connection address. Platform TLS retains `MBEDTLS_SSL_VERIFY_REQUIRED`.
The HTTP adapter preserves an explicitly supplied Host exactly once, supplies
the port for a nondefault automatic Host, and passes the configured connection
and read timeouts. Existing platform entry points keep their direct behavior.

A changed DHCP address invalidates the route decision with the official
nonblocking reset. The lookup runs in the next requesting task, outside the
network callback. The upstream component retains its region cache, retries,
three-failure/ten-minute direct fallback, and custom-gateway bypass.

## Validation and limits

Run `sh arcs/tests/host/run_cloud_route.sh` for the real ARCS HTTP/TLS adapter
harness and upstream route-policy tests, without hardware or an account.
Set `ARCS_BASE=/path/to/arcs-sdk-0.1.6` to additionally execute the real mbedTLS
configuration with a stubbed socket/handshake: peer/SNI separation, required
certificate verification, the direct wrapper, and verification-error propagation.
This is a configuration/failure-path test, not a live certificate-chain test.

Before the upstream `gw.itoy.cn` change, earlier ARCS Mini firmware confirmed mainland
selection, three reset business TLS connections (`-0x0050`, errno 104), and the
official ten-minute direct fallback. Direct registration and voice subscription
then succeeded; a person confirmed screen, speech and continuous conversation
on that direct path. The older relay business connection did not pass. These results
do not identify the reset's cause or establish long-term reliability, and this
upstream-only branch has not been flashed as part of PR preparation.
