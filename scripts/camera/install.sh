#!/bin/sh
#
# Installs PixelPilot's camera-side pieces on an OpenIPC air unit. Runs ON the camera, from
# the directory it was copied to together with pixelpilot-status and pixelpilot-osd - see
# deploy.sh, which does both. Safe to run again: every step checks for its own result first,
# so it is also how the pieces are brought back after a firmware update.
#
#   MSPOSD_EXTRA="-M -a 3" sh install.sh
#
# MSPOSD_EXTRA is what msposd gets besides the stock arguments and the OSD switch. -M makes it
# read MAVLink, which is only right when the flight controller's port is set to MAVLink - and
# then it is load-bearing: msposd is the only reader of that port, so without -M there is no
# telemetry at all. -a 3 is its artificial horizon. Leave MSPOSD_EXTRA empty for an FC that
# speaks MSP.

set -e
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

HERE=$(cd "$(dirname "$0")" && pwd)
MSPOSD_EXTRA=${MSPOSD_EXTRA-"-M -a 3"}
WB=/usr/bin/wifibroadcast
RC=/etc/rc.local

say() { echo "pixelpilot: $*"; }

# 0. What this msposd understands. -M is what the telemetry depends on (see above), so a
#    firmware whose msposd no longer has it is worth stopping for rather than patching in.
if [ -n "$MSPOSD_EXTRA" ] && echo " $MSPOSD_EXTRA " | grep -q ' -M ' && ! msposd --help 2>&1 | grep -q -- '--mavlink'; then
	say "this msposd has no --mavlink: telemetry would stop - not installing. See README.md."
	exit 1
fi
say "msposd: $(msposd --help 2>&1 | awk '/^Version/ {print $2; exit}'), OSD flag $(msposd --help 2>&1 | awk '/--osd/ {print $1; exit}')"

# 1. The two scripts.
for f in pixelpilot-status pixelpilot-osd; do
	tr -d '\r' < "$HERE/$f" > "/usr/bin/$f.new"
	chmod 755 "/usr/bin/$f.new"
	mv "/usr/bin/$f.new" "/usr/bin/$f"
	say "installed /usr/bin/$f"
done

# 2. The msposd line in wifibroadcast: -d (burn the OSD in) comes from pixelpilot-osd, so the
#    remembered choice holds from boot on. Falls back to -d if the script is ever missing.
if grep -q 'pixelpilot-osd flag' "$WB"; then
	say "$WB already asks pixelpilot-osd"
else
	[ -f "$WB.orig" ] || cp "$WB" "$WB.orig"
	if awk -v extra="$MSPOSD_EXTRA" '
		index($0, "-z \"$size\"") > 0 && index($0, "port_tx") > 0 && !done {
			i = index($0, "-z \"$size\"")
			$0 = substr($0, 1, i - 1) "-z \"$size\" $(pixelpilot-osd flag 2>/dev/null || echo -d) " extra " > /dev/null &"
			done = 1
		}
		{ print }
		END { if (!done) exit 3 }' "$WB" > "$WB.new"; then
		chmod 755 "$WB.new"
		mv "$WB.new" "$WB"
		say "patched the msposd line in $WB:"
		grep -n 'pixelpilot-osd flag' "$WB"
	else
		rm -f "$WB.new"
		say "could not find the msposd line in $WB - patch it by hand, see README.md"
	fi
fi

# 3. Started at boot. The status file is served from RAM, not from /var/www, which is flash.
[ -f "$RC.orig" ] || cp "$RC" "$RC.orig"
add_rc() {
	if grep -qF "$1" "$RC"; then
		say "rc.local already has: $1"
	else
		awk -v line="$1" '/^exit 0/ && !done { print line; done = 1 } { print } END { if (!done) print line }' "$RC" > "$RC.new"
		mv "$RC.new" "$RC"
		chmod 755 "$RC"
		say "rc.local: added $1"
	fi
}
# The first version served the status from /var/www/pp, which is flash; that line goes, and
# the one below serves it from RAM instead.
if grep -q 'httpd -p 8080 -h /var/www/pp' "$RC"; then
	sed -i '\|httpd -p 8080 -h /var/www/pp|d' "$RC"
	say "rc.local: dropped the status web server on flash"
fi
add_rc 'pixelpilot-status &'
add_rc 'mkdir -p /tmp/pixelpilot && busybox httpd -p 8080 -h /tmp/pixelpilot &'
rm -rf /var/www/pp

# 4. Running now, without a reboot: the status pusher restarted, the web server on RAM.
for pid in $(pidof pixelpilot-status) $(ps | awk '/[p]ixelpilot-status/ {print $1}'); do
	kill "$pid" 2>/dev/null || true
done
for pid in $(ps | awk '/[h]ttpd -p 8080/ {print $1}'); do
	kill "$pid" 2>/dev/null || true
done
mkdir -p /tmp/pixelpilot
setsid pixelpilot-status </dev/null >/dev/null 2>&1 &
setsid busybox httpd -p 8080 -h /tmp/pixelpilot </dev/null >/dev/null 2>&1 &
sleep 1
say "running: $(ps | grep -cE '[p]ixelpilot-status') status pusher, OSD $(pixelpilot-osd state) (remembered: $(pixelpilot-osd mode))"
