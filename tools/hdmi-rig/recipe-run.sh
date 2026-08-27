#!/usr/bin/env bash
# recipe-run.sh N [ARG] — run one numbered HDMI recipe on the badge.
#
# Wraps on-badge/recipe.sh, which must already be installed on the badge at
# /home/debian/badgehdmi/recipe.sh (see README.md "Provisioning the badge").
# Recipe numbers are documented in on-badge/recipe.sh; the everyday ones:
#   4  landscape white baseline (720p sync anchor)
#  10  hardware colorbar via VO_IOCTL_PATTERN (no producer needed)
#
# Env: BADGE_HOST / BADGE_USER / BADGE_PASS as in badge-run.sh.
# The sudo password is auto-supplied — do not type anything, just wait.
set -u
N="${1:-4}"
ARG="${2:-}"
BADGE_HOST="${BADGE_HOST:-192.168.1.228}"
BADGE_USER="${BADGE_USER:-debian}"
BADGE_PASS="${BADGE_PASS:-rv}"

exec sshpass -p "$BADGE_PASS" \
  ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 \
  "$BADGE_USER@$BADGE_HOST" \
  "echo $BADGE_PASS | sudo -S -p '' bash /home/debian/badgehdmi/recipe.sh $N $ARG"
