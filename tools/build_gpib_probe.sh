#!/bin/bash
# Builds tools/gpib_probe.
#
# No entitlement any more: libgpib reaches the driver through the gpibd
# broker, so this is an ordinary unprivileged binary. That is the whole point
# of the broker — before it, this tool needed
# com.apple.developer.driverkit.userclient-access just to read a bus line.
#
# Requires gpibd to be registered:  gpibd/build.sh && gpibd/install.sh
set -euo pipefail
cd "$(dirname "$0")"

clang -O2 -o gpib_probe gpib_probe.c
codesign --force --sign - gpib_probe

echo "built (no entitlements): $(pwd)/gpib_probe"
