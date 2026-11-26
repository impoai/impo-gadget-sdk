#!/usr/bin/env python3
"""Reject incomplete ARCS link products before a CP image is considered usable."""
import argparse
from pathlib import Path

REQUIRED = (
    "__lisa_device_registry_gpioa", "__lisa_device_registry_gpiob",
    "__lisa_device_registry_audio0", "__lisa_device_registry_pwm0",
    "__lisa_device_registry_spi0", "__lisa_device_registry_display",
    "__lisa_device_registry_st7789p3", "arcs_mini_wake_start",
    "gadget_voice_wake", "gadget_voice_continuous_active", "preferred_boot_task",
    "impo_cloud_route_endpoint", "impo_cloud_route_url", "impo_cloud_route_tls_name",
    "impo_cloud_route_report", "impo_cloud_route_reset", "plat_tls_connect_named",
    "impo_arcs_mini_product", "impo_arcs_mini_application",
)

def incomplete_archives(build):
    bad = []
    for archive in Path(build).rglob("*.a"):
        with archive.open("rb") as f:
            magic = f.read(8)
        if magic not in (b"!<arch>\n", b"!<thin>\n"):
            bad.append(archive)
    return bad

def check(build, wake_enabled=True):
    build = Path(build)
    bad = incomplete_archives(build)
    if bad:
        raise SystemExit("Incomplete generated archive: " + str(bad[0]))
    symbols = (build / "impo-gadget.symb").read_text()
    required = REQUIRED + (("impo_arcs_wake_resources", "impo_arcs_wake_result") if wake_enabled else ())
    missing = [name for name in required if name + "|" not in symbols.replace(" ", "")]
    if missing:
        raise SystemExit("Required hardware/feature symbols missing: " + ", ".join(missing))
    size = (build / "impo-gadget.bin").stat().st_size
    if not 512 < size <= 0x400000:
        raise SystemExit("Image does not fit the CP partition")
    print("Image check: archives, device registry, SDK hooks, wake/dialogue and CP size passed")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("build", type=Path)
    ap.add_argument("--no-wake", action="store_true")
    a = ap.parse_args()
    check(a.build, wake_enabled=not a.no_wake)
