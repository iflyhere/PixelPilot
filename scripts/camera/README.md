# Camera-side pieces for PixelPilot's headset mode

Two small scripts that run on the OpenIPC air unit, and the installer that puts them there.
Nothing in the app depends on them: with a stock camera everything still works, the app just
cannot read the camera's temperature or switch the OSD the camera burns into the video.

| File | Runs on | What it does |
|---|---|---|
| `pixelpilot-status` | camera, from `/etc/rc.local` | Every 2 s pushes temperature, CPU, memory, air throughput and the OSD state to the headset (TCP `10.5.0.3:9099`), and applies the OSD the app answers with. Also keeps the same block at `http://<camera>:8080/pixelpilot.txt` for a shell. |
| `pixelpilot-osd` | camera | Switches msposd's burned-in OSD on or off (a restart with or without `-d`) and remembers the choice in `/etc/pixelpilot-osd.mode`. |
| `install.sh` | camera | Installs both, patches the msposd line in `/usr/bin/wifibroadcast`, adds the boot lines to `/etc/rc.local`. Safe to run again. |
| `deploy.sh` | your PC | Copies the three files onto the camera and runs `install.sh`. |

## Why the camera pushes instead of the app fetching

Android keeps a `VpnService` owner out of its own tunnel, so the app cannot open a connection
to the camera at `10.5.0.10` - a shell on the same headset can, the app times out. An inbound
connection to a listening socket is not route filtered, so the camera connects to the app, and
the app's answer on that same connection is the only way back. That is how the OSD choice
reaches the camera.

## The OSD switch

The headset draws its own OSD from the same MAVLink stream msposd draws from, so the app's
VR settings offer:

* **Camera** - burned into the video by msposd. The app hides its own symbology over the video.
* **Headset** - drawn by the app. The camera stops burning it in; the video stays clean, and so
  does every recording made from it.
* **Both** - the default, which is what happened before the switch existed.

The map, the dashboard and the chart beside the video are the app's in every mode.

msposd cannot change this while it runs - this version has no file, signal or command for it -
so `pixelpilot-osd` restarts it with the arguments it was running with, minus or plus `-d`.
Telemetry keeps flowing without `-d`; measured on the camera, msposd forwarded 25 packets a
second to the telemetry link without it against 18 with it, and 2 with msposd stopped. A
switch costs about a second of telemetry and takes up to 4 s to arrive (the 2 s push interval
plus the restart).

`pixelpilot-osd` with no mode file means **camera**, which is what a camera did before this. At
boot the patched `wifibroadcast` asks it for the `-d` flag, so the choice survives a reboot.

**Compatibility.** A camera without these scripts never sends `osd_switch=1`, the app then only
switches its own layer and says so. An older `pixelpilot-status` ignores the app's answer. An
older app does not answer, and the camera's OSD stays as it is.

## Installing, and restoring after a firmware update

A firmware update replaces the overlay (`/overlay`, mounted over `/`), so **everything below is
lost** and has to be put back. In order:

1. **Flash** the new firmware, then set the root password again.
2. **Radio and video settings.** These are the values this was developed with; take them from
   the camera's own UI or `cli`/the YAML files, not by copying old files over new ones - a new
   firmware may have moved keys.

   `/etc/wfb.yaml`: `wireless.txpower 40`, `channel 161`, `width 20`, `mlink 3994`,
   `wlan_adapter bl-m8812eu2`, `link_control alink`; `broadcast.mcs_index 2`, `fec_k 8`,
   `fec_n 12`, `stbc 1`, `ldpc 1`, `link_id 7669206`; `telemetry.router msposd`,
   `serial ttyS2`, `osd_fps 20`.

   `/etc/alink.conf`: `power_level_0_to_4=3` (stock is 0, pit mode).

   `/etc/majestic.yaml`: `video0` `codec h265`, `fps 60`, `bitrate 19000`, `rcMode cbr`,
   `gopSize 10`, `size 1920x1080`, `minQp 12`, `maxQp 48`, `qpDelta -12`; `isp.exposure 16`,
   `isp.sensorConfig /etc/sensors/imx415_infinity6e.bin`; `fpv.enabled true`;
   `outgoing.enabled true`, `outgoing.wfb true`, `outgoing.naluSize 3894`;
   **`audio.enabled false`** - with audio on, this firmware's majestic has crashed in flight.

   `/etc/rc.local`: `alink_drone &` before `exit 0` - but only if it is missing. The
   2026-10-15 firmware writes it there itself whenever `link_control` is `alink`; started
   twice, two adaptive-link daemons fight over the bitrate. `install.sh` adds only its own
   lines.

   **Reboot once after the first boot of a fresh firmware.** On that first boot
   `wifibroadcast` writes STBC/LDPC for the adapter it finds into `wfb.yaml`, but the radio has
   already been started with them off (`wfb_tx -S 0 -L 0`).
3. **Flight controller:** the UART msposd reads (`ttyS2`) must be set to **MAVLink** in
   Betaflight. msposd runs with `-M` here, and it is the only reader of that port: without
   MAVLink on it there is no telemetry in the headset at all.
4. **Run `deploy.sh`** from this directory:

   ```bash
   scripts/camera/deploy.sh
   ```

   On Windows with PuTTY: `SSH="plink -batch -ssh -pw <password>" scripts/camera/deploy.sh`.
   For an FC that speaks MSP instead of MAVLink: `MSPOSD_EXTRA="" scripts/camera/deploy.sh`.
5. **Check:**

   ```bash
   ssh root@192.168.1.10 'ps | grep [m]sposd; pixelpilot-osd state; cat /tmp/pixelpilot/pixelpilot.txt'
   ```

   msposd should run with `-M -a 3` (and `-d` for the camera OSD), and the status block should
   end in `osd_switch=1` / `osd_mode=...`. In the headset the dashboard shows the camera's
   temperature within a few seconds.

If `install.sh` cannot find the msposd line in a new `wifibroadcast`, it says so and changes
nothing there. The edit by hand: on the msposd line, replace whatever follows `-z "$size"`
with `$(pixelpilot-osd flag 2>/dev/null || echo -d) -M -a 3 > /dev/null &`.

## What the update on 2026-10-16 actually needed

From `nightly-20260727-6c0a45e` to `nightly-20261015-214075f` (OpenIPC 2.6.10.15, platform
`ssc338q_wfbng_openipc-urllc-aio`), with `sysupgrade -k -r -n`:

* `wfb.yaml` already had every radio value above - the platform defaults now match.
* `drone.key` unchanged, so the headset's `gs.key` still fits.
* majestic kept codec, 60 fps, CBR, size, sensor, exposure and `fpv`; it needed `video0.bitrate
  19000` (was 8000), `gopSize 10`, `minQp 12`, `maxQp 48`, `qpDelta -12`, `outgoing.enabled
  true`, `outgoing.naluSize 3894` and `audio.enabled false` (set with `cli -s`).
* `alink.conf` was back at `power_level_0_to_4=0`.
* msposd ad5ec36: still `-d` for the OSD and `-M` for MAVLink; `install.sh` patched the new
  `wifibroadcast` without help.
* The SSH host key is new, as after every flash with `-n`.

## Undoing it

`install.sh` keeps `/usr/bin/wifibroadcast.orig` and `/etc/rc.local.orig`. Put those back,
delete `/usr/bin/pixelpilot-status`, `/usr/bin/pixelpilot-osd` and `/etc/pixelpilot-osd.mode`,
and reboot.
