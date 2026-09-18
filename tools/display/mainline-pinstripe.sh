#!/usr/bin/env bash
# Runs on the selected test image; never called by a boot service.
set -euo pipefail
case ${1:-} in start | stop | status) ;; *)
	echo 'usage: mainline-pinstripe start|status|stop' >&2
	exit 2
	;;
esac
control=/sys/bus/platform/devices/a080000.dsi/pinstripe
if [[ ! -r $control ]]; then
	echo 'pinstripe: driver/board graph not bound; inspect dmesg and /sys/kernel/debug/devices_deferred' >&2
	exit 1
fi
result=0
if [[ $1 != status ]]; then
	printf '%s\n' "$1" >"$control" || result=$?
fi
cat "$control"
exit "$result"
