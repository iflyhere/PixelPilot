#!/usr/bin/env bash
#
# Copies scripts/camera onto the air unit and runs install.sh there. From any machine with
# ssh; on Windows, Git Bash with PuTTY's plink works too:
#
#   scripts/camera/deploy.sh                       # ssh root@192.168.1.10
#   CAMERA=root@10.5.0.10 scripts/camera/deploy.sh # over the wfb-ng tunnel
#   SSH="plink -batch -ssh -pw 12345" scripts/camera/deploy.sh
#
# Nothing here needs scp or sftp, which a camera's dropbear may not have: every file goes in
# as a cat, with its CRs stripped - a Windows checkout has CRLF, and a shell script with CRs
# dies at its first line.

set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CAMERA=${CAMERA:-root@192.168.1.10}
SSH=${SSH:-ssh}
DEST=/tmp/pixelpilot-camera

# shellcheck disable=SC2086 - SSH may carry options
$SSH "$CAMERA" "mkdir -p $DEST"
for f in install.sh pixelpilot-status pixelpilot-osd; do
	tr -d '\r' < "$HERE/$f" | $SSH "$CAMERA" "cat > $DEST/$f"
done
$SSH "$CAMERA" "MSPOSD_EXTRA='${MSPOSD_EXTRA--M -a 3}' sh $DEST/install.sh"
