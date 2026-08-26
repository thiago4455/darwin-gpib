#!/bin/bash
# Builds the gpibd broker and the gpibctl client.
#
# Phase 1 builds outside Xcode on purpose: adding a target means editing
# project.pbxproj, and there is no reason to restructure the project before the
# design's load-bearing assumption is proven. Once it is, gpibd becomes a
# proper target shipped in Open GPIB.app/Contents/Library/LoginItems/.
#
# gpibd is ad-hoc signed WITH the DriverKit user-client entitlement.
# gpibctl is signed WITHOUT it, deliberately — it is the test subject.
#
# gpibctl lives in tools/ rather than here: it is a client of the broker, not
# part of it, and the gpibd target's synchronized group would otherwise
# compile both mains into one binary.
set -euo pipefail
cd "$(dirname "$0")"

CFLAGS="-O2 -fblocks -Wall -Wextra -Wno-unused-parameter -I. -I../driver/gpib"
FRAMEWORKS="-framework IOKit -framework CoreFoundation -framework Foundation"

clang $CFLAGS -o gpibd            gpibd.c          $FRAMEWORKS
clang $CFLAGS -o ../tools/gpibctl ../tools/gpibctl.c -framework Foundation

codesign --force --sign - --entitlements gpibd.entitlements gpibd
codesign --force --sign - ../tools/gpibctl

# launchd keeps the previously-launched binary resident, so a rebuild is not
# picked up until the running instance goes away. Killing it is safe: the next
# client message relaunches from the new binary.
if pkill -x gpibd 2>/dev/null; then
    echo "stopped the running gpibd so the rebuild takes effect"
fi

echo "built:"
echo "  gpibd   (entitled)   $(pwd)/gpibd"
echo "  gpibctl (unentitled) $(cd ../tools && pwd)/gpibctl"
