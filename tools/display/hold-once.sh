#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# One activation of already-loaded candidate modules. No deployment or reboot.
set -uo pipefail
export PATH=/run/current-system/sw/bin
fail() { printf 'DISPLAY_REFUSED: %s\n' "$*" >&2; exit 1; }
[[ $# == 5 ]] || fail 'usage: hold-once.sh PREVIOUS_BOOT FRESH_BOOT DSI_NOTE_SHA BRIDGE_NOTE_SHA LIVE_FDT_SHA'
previous=$1; expected_boot=$2; dsi_note=$3; bridge_note=$4; dt_sha=$5
for value in "$previous" "$expected_boot"; do
    [[ $value =~ ^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$ ]] || fail 'invalid boot ID'
done
for value in "$dsi_note" "$bridge_note" "$dt_sha"; do
    [[ $value =~ ^[0-9a-f]{64}$ ]] || fail 'invalid SHA-256'
done
[[ $EUID == 0 && $(uname -m) == riscv64 && $(uname -r) == 7.2.2 ]] || fail 'root/riscv64/Linux 7.2.2 required'
boot=$(cat /proc/sys/kernel/random/boot_id) || fail 'boot identity unreadable'
[[ $boot == "$expected_boot" && $boot != "$previous" ]] || fail 'fresh boot mismatch'
hash() { sha256sum "$1" | cut -d ' ' -f1; }
[[ $(hash /sys/module/sophgo_dsi/notes/.note.gnu.build-id) == "$dsi_note" ]] || fail 'DSI note mismatch'
[[ $(hash /sys/module/lontium_lt8912b/notes/.note.gnu.build-id) == "$bridge_note" ]] || fail 'bridge note mismatch'
[[ $(hash /sys/firmware/fdt) == "$dt_sha" ]] || fail 'live FDT mismatch'
for param in pinstripe_trace pinstripe_snapshot pinstripe_dphy mac_fire_and_forget vip_bt_clock; do
    [[ $(cat /sys/module/sophgo_dsi/parameters/"$param") == Y ]] || fail "parameter $param not enabled"
done
[[ $(cat /sys/module/sophgo_dsi/parameters/mac_no_eot) == N ]] || fail 'mac_no_eot must be disabled'
bridges=$(cat /sys/kernel/debug/dri/bridges) || fail 'bridge list unavailable'
[[ $bridges == *'[lontium_lt8912b]'* && $bridges == *'[sophgo_dsi]'* ]] || fail 'bridge chain missing'
idle='selected=1 terminal=0 started=0 owned=0 stage=idle error=0'
owned='selected=1 terminal=0 started=1 owned=1 stage=idle error=0'
status() { timeout -k 2 5 mainline-pinstripe status; }
[[ $(status) == "$idle" ]] || fail 'pipeline not idle'
marker=/run/badge-display-start-attempted
# Share the historical start marker, so older helpers also refuse this boot.
for old in "$marker" /run/badge-r*-start-attempted /run/badge-r20-cycles /run/badge-r20-stop-attempted; do
    [[ ! -e $old && ! -L $old ]] || fail 'activation marker already exists'
done
[[ -r /sys/kernel/debug/sophgo-dsi/pinstripe-registers ]] || fail 'snapshot unavailable'
umask 077
mkdir /run/badge-r9-start-attempted || fail 'concurrent legacy activation'
mkdir "$marker" || fail 'concurrent public activation'
attempted=0
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [[ $attempted == 1 ]]; then
        state=$(status 2>&1); state_rc=$?
        printf 'DISPLAY_STATUS_BEFORE_STOP=%s\n' "$state"
        if [[ $state_rc == 0 && $state == "$owned" ]]; then
            # Exactly one stop. A terminal/unknown state never triggers a retry.
            timeout -k 2 15 mainline-pinstripe stop; stop_rc=$?
            printf 'DISPLAY_STOP_EXIT=%s\n' "$stop_rc"
            state=$(status 2>&1); state_rc=$?
            [[ $stop_rc == 0 && $state_rc == 0 && $state == "$idle" ]] || result=1
        elif [[ $state_rc != 0 || $state != "$idle" ]]; then
            printf 'DISPLAY_STOP_REFUSED: terminal/unknown state; operator recovery required\n' >&2
            result=1
        fi
        printf 'DISPLAY_FINAL_STATUS=%s\n' "$state"
    fi
    printf 'DISPLAY_END exit=%s\n' "$result"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
attempted=1
printf 'DISPLAY_START boot=%s\n' "$boot"
timeout -k 2 25 mainline-pinstripe start; start_rc=$?
printf 'DISPLAY_START_EXIT=%s\n' "$start_rc"
[[ $start_rc == 0 && $(status) == "$owned" ]] || fail 'start did not reach owned state'
printf 'DISPLAY_HOLD_BEGIN seconds=45\n'
sleep 45 || fail 'hold interrupted'
[[ $(status) == "$owned" ]] || fail 'ownership changed during hold'
# Read cached prepared/started/complete slots ONCE. This performs no new MMIO;
# a late read cannot establish late D-PHY state or qualify timed divergence.
blob="$marker/snapshot.bin"
timeout -k 2 5 cat /sys/kernel/debug/sophgo-dsi/pinstripe-registers > "$blob" || fail 'snapshot read failed'
[[ $(wc -c < "$blob") == 74000 ]] || fail 'snapshot layout size'
printf 'DISPLAY_CACHED_SNAPSHOT sha256=%s bytes=74000\n' "$(hash "$blob")"
printf 'DISPLAY_BLOB_BEGIN\n'
base64 "$blob" || fail 'snapshot transport failed'
printf 'DISPLAY_BLOB_END\nDISPLAY_HOLD_END\n'
# EXIT cleanup performs the one stop even after a snapshot/transport failure.
exit 0
