#!/usr/bin/env bash
# badge-run.sh LOCAL_SCRIPT.sh — run an arbitrary shell script on the badge as root.
#
# Target is the badge running the scpcom Debian vendor image (the HDMI bring-up
# reference rig, issue #4). Connection settings come from the environment:
#   BADGE_HOST  badge IP/hostname            (default 192.168.1.228)
#   BADGE_USER  ssh user                     (default debian, the image default)
#   BADGE_PASS  ssh + sudo password          (default rv, the image default login)
#
# The stdin trick: feed the sudo password line AND the script body through ONE
# stdin. The naive `echo pw | sudo -S bash -s < script` fails — sudo -S consumes
# the pipe (the password) and bash -s then reads an empty script. Concatenating
# `{ echo pw; cat script; }` gives sudo its password on line 1 and bash the rest.
# This also avoids every nested ssh/sudo quote-mangling problem.
#
# Usage: badge-run.sh /path/to/script.sh
set -u
SCRIPT="${1:?usage: badge-run.sh LOCAL_SCRIPT.sh}"
BADGE_HOST="${BADGE_HOST:-192.168.1.228}"
BADGE_USER="${BADGE_USER:-debian}"
BADGE_PASS="${BADGE_PASS:-rv}"

{ echo "$BADGE_PASS"; cat "$SCRIPT"; } | sshpass -p "$BADGE_PASS" \
  ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 \
  "$BADGE_USER@$BADGE_HOST" 'sudo -S -p "" bash -s'
