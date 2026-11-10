# The `clarion-qy7` machine

`clarion-qy7` models the QY7221NL SH-4A unit. The `qemu-clarion` launcher
selects the machine from the NOR image and can keep both the NOR and SD card
unchanged with `--read-only`:

```sh
build-release/qemu-clarion --read-only --headless qy7-nor.bin map-card.img \
    -qmp unix:/tmp/qy7.sock,server=on,wait=off
```

Omit `--headless` to use the display window. The QY7 guest takes longer to
reach its display than the QY8 guest. The machine connects its display
microcontroller to SCIF3; panel input is represented as guest protocol frames.

## GPS receiver

The QY7 receiver sends A0 frames on SCIF7. QEMU generates the receiver's
calendar fields from the guest virtual clock. Use `-rtc base=...,clock=vm` to
choose a repeatable base date; `clock=vm` advances that date with guest time.
The `gps-rollover` machine property controls the modeled 1024-week date offset
(default `on`). Set it to `off` when testing an unshifted date.

The machine properties are:

| Property | Meaning |
|---|---|
| `gps` | Enable the receiver. |
| `gps-status` | A0 status byte; `0x20` is accepted by the Navi position path. |
| `gps-lat`, `gps-lon` | Current receiver position in decimal degrees. |
| `gps-speed-kmh` | Current GPS ground speed in km/h. |
| `gps-course` | Compass course in degrees: north is 0 and angles increase clockwise. |
| `gps-rollover` | Apply the 1024-week offset to the receiver date. |

The model encodes speed in 0.1 km/h units and converts compass course to Navi's
Sensor info frame convention (east is zero; course increases counterclockwise).
`+0x16` HDOP is 1. The startup sequence sends one time-only frame with status
`0x01`, then changes to `0x20` after the first A0 frame is accepted.

Start QY7 with a QMP socket and a virtual RTC. This example leaves the machine's
configured starting position in place; choose a map card that covers that
position, or set `gps-lat`/`gps-lon` to a position on the selected card before
starting the route player.

```sh
build-release/qemu-clarion --read-only --headless qy7-nor.bin map-card.img \
    -M clarion-qy7,gps=on,gps-status=32,gps-course=90,gps-rollover=off,wdt=off,dipsw=7 \
    -rtc base=2026-11-03T12:00:00,clock=vm \
    -icount shift=2,sleep=off \
    -qmp unix:/tmp/qy7.sock,server=on,wait=off
```

Wait for the map, then enable follow mode. If a map tap moved the view, use the
upper `TILB` control to return to the vehicle-follow view. QMP screenshots can
be saved while QEMU is headless.


## Gyro ADC inputs

QY7 `navdrv` maps `HPB_GYRO_REGBASE` at `0xFFC5A000` and reads ADC channels
from direct 32-bit registers. The A7 alias at `0x1FC5A000` maps the same
register region. The current model returns:

| Offset | Channel | Value | Evidence |
|---|---:|---:|---|
| `+0x030` | 0 | `gyro-raw` (default `0x5000`) | `SckRawCheck` range `(0x3CC9, 0x632D)` at `navdrv.dll:0x03CD5D4C–0x03CD5D5A` |
| `+0x034` | 1 | `0x5000` | Shared `SckRawCheck` range with channel 2: `(0x1FFE, 0x7FF8)` |
| `+0x038` | 2 | `0x5000` | Shared `SckRawCheck` range with channel 1: `(0x1FFE, 0x7FF8)` |
| `+0x03C` | 3 | `0x4000` | `SckRawCheck` range `(0x1FFE, 0x5FFA)` |
| `+0x040` | 4 | `0x4000` | Read by the gyro worker; no `SckRawCheck` range established |

These are candidate passing values from the stationary QY7 control. The
firmware checks open intervals and contains no repeated-sample counter in
`SckRawCheck`. ADC0 is inferred to be the yaw gyro. ADC1 and ADC2 are inferred
to be the two-axis G-sensor, consistent with the firmware string
`GSNS 2 AXIS`; ADC3 is inferred to be a reference voltage or temperature
input. These physical roles are not proven. Initialization writes are
accepted and logged with `LOG_UNIMP`.

## Reverse and illumination inputs

`navdrv` maps the board STATUS1 register (`0x19800000`, uncached alias
`0xB9800000`, `navdrv.dll:0x03CD6BEC`) and keeps its low three bits as the
"IPR" signal word (`SensorThread`, `0x03CD6A56–0x03CD6A5E`; the word is split
into ILL, bit 0 and bit 2 fields by `0x03CD5F58`).  The word is read once
when the sensor thread starts and again on each IPR event; the model does not
raise that event, so changes made after boot may not be sampled.  Set the
properties on the command line.

| STATUS1 bit | Property | Polarity | Evidence |
|---|---|---|---|
| 0 | `reverse` (default off) | active low | Boot A/B with only this bit changed: bit 0 clear makes the Navi dead-reckoning step run opposite to the arrow (about 180 degrees); bit 0 set makes it follow the arrow. The direction decision itself is in `Navi.exe`; `navdrv` only forwards the bit. |
| 1 | `illumination` (default off) | active low | `InitIllSetting` (`0x03CD690C`) in auto mode derives ILL as `~(signal >> 1) & 1`; with the bit clear the map uses the night palette once movement starts. |
| 2 | not modelled (reads 0) | unknown | Boot A/B (bit 2 set or clear): no visible effect on direction or palette. |

STATUS1 therefore reads `0x0013` by default (bit 4 is the previous constant).
Before this change bits 0 and 1 read as zero, which asserted both inputs.

## Speed pulse input

The QY7 SPDPLS model uses `speed-kmh` and `vehicle-pulses-per-km` (default
5096) to synthesize pulse edges on the virtual clock. The firmware conversion
constants at `Navi.exe` offsets `0x003a5d00` and `0x003a5cac` establish the
5096 pulses/km scale. `odometer-m` reports cumulative traveled distance in
meters since QEMU started.

| SPDPLS offset | Model behavior |
|---|---|
| `+0x004` | Read/write filter value. |
| `+0x008` | Writing bit 0 clears the pulse count, latch, and 100 ms tick count. |
| `+0x00c` | Low 16 bits of the cumulative pulse count, latched every 100 ms. |
| `+0x010` | Number of 100 ms latch ticks; the Navi handler runs once per tick and updates its speed estimate every ten ticks. |
| `+0x018` | Status value; writing bit 0 clears it. |
| `+0x01c` | 500 kHz frequency register, readable and writable; defaults to zero. |
| `+0x104`, `+0x110` | Modeled zero-valued registers; writes are not modeled. |

The model raises INT2 bit 22 at each 100 ms latch. With `speed-kmh=0`, no
pulse timer runs; the periodic latch and interrupt remain active. These are
emulator model semantics based on the current device implementation; they do
not claim undocumented physical register behavior.

## Driving a route

`speed-kmh` drives SPDPLS and advances `odometer-m`. Positive `yaw-rate` turns
the vehicle right. The `scripts/clarion/drive_route.py` player follows the
read-only odometer and updates `gps-lat`, `gps-lon`, `gps-speed-kmh`, and
`gps-course` at each poll. The GPS position is translated from the route's
geometry to the machine's current starting position. The course supplied by a
route is compass-based; the GPS model performs the Navi course conversion.

A JSON route uses local east/north offsets in meters rather than geographic
coordinates. Each point sets the speed for the segment that starts there.
`initial_heading` is the vehicle's direction before the first route segment;
keep it aligned with the vehicle at the start. For GPX input, the player uses
trackpoint distances and bearings, and uses timestamps to derive segment speed
when available; otherwise it uses `--speed`. The GPX geometry is also replayed
from the machine's current GPS starting point.

Example `docs/clarion/qy7-example-route.json` is a synthetic four-segment loop.
Start QEMU as above, wait for the map and enable follow mode, then run:

```sh
python3 scripts/clarion/drive_route.py /tmp/qy7.sock \
    --route docs/clarion/qy7-example-route.json \
    --marks 0,400,700,1100,1400 --out route-output
```

The player saves a PNG at each requested route distance and a `track.jsonl`
log with distance, segment, speed, course, and yaw rate. It sets vehicle and
GPS speeds to zero at the end. Check that the odometer advances approximately
the route length and that the marker follows the route and turns in the
expected direction. In the diagnostic menu with DIPSW=4, Sensor info shows
`Vgps` and `Dgps`; these are the GPS speed and Navi-converted course values.


## Front-panel keys

QEMU key-down and key-up events become command `0x34` frames. The high bit in
the data byte marks key-down; the low seven bits are the panel key code. The
frame uses the display-micom `DLE STX … DLE ETX` framing and BCC, and the model
waits for the guest's `DLE ACK` before completing the key event.

| Host key | Panel control | Panel code | Evidence |
|---|---|---:|---|
| `D` | DAY/NIGHT | `0x01` | Active KEPDRV tables map `01 -> 01 -> 0x22`; on the map the guest reports `SK_HWK_DAY_NIGHT_CLICK`. |
| `B` | Brightness/day-night control (inferred) | `0x21` | Active tables map `21 -> 02 -> 0xD9`; an early UI run showed `Lysstyrke` for this code under the previous host alias `S`. |

Host keys follow one table for QY7 and QY8 (see the QY8 reference, “Panel
keys and knobs”): the same key presses the same function. On QY7 only the
codes above and the MCAN routes below are proven; other keys of the table
(`Esc`, `Enter`, `C`, `A`, `[`/`]`, F1–F9, knobs) send nothing here.

The active QY7 `kepdrv.dll` table observed after boot has two first-stage
entries, `01 -> 01` and `21 -> 02`, and two second-stage entries,
`01 -> 0x22` and `02 -> 0xD9`. Both codes reach `AuiKeyEvent`; the first
translation is VK `0x22`, while the second is `0xD9`. The displayed brightness
overlay associated code `0x21` with brightness/day-night control in an early
UI run. The final replay stayed in a blank post-consent transition, so those
visible effects were not reproduced. The physical button label remains an
inference from the panel photo.

MENU, MAP/VOICE, STATUS and Zero Emission also arrive over the vehicle CAN
bus as MCAN `PUSH_SW` frames; see
[Verified MCAN PUSH_SW routes](#verified-mcan-push_sw-routes). Codes for
TUNE/SEEK/CH, FM-AM, XM, CD-AUX, VOL+/VOL−, CAMERA, and OPEN/TILT have
not been established. Do not infer codes for them from their labels.

Send a key through QMP in headless mode:

```json
{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"m"}],"hold-time":100}}
```

The same host keys can be pressed in the QEMU display window.

## Raw panel-frame diagnostic

The QY7 display-micom exposes a persistent write-only `key-raw` QOM property
for investigating panel commands whose meaning is unknown. Its value is the
frame body as hexadecimal bytes, not a complete framed packet. The model adds
DLE/STX, DLE escaping, DLE/ETX, and BCC, sends the bytes through SCIF3, and
waits for DLE ACK (retrying up to three times on timeout or NAK).

For example, inject the tested one-byte `0x34` body and a four-byte `0x36`
body:

```json
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"34 81"}}
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"36 86 60 01 00"}}
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"36 83 3B FF 01"}}
{"execute":"qom-set","arguments":{"path":"/machine/qy7-dispmicom","property":"key-raw","value":"36 86 6A 01 00"}}
```

Spaces, commas, and colons may separate bytes. One to 64 bytes are accepted.
The diagnostic does not validate command semantics, so use it with a
read-only guest run. It is enabled only on `clarion-qy7`; QY8 does not enable
the key input or this property.

## CAN receive, logging, and replay

The QY7 machine exposes the same CAN properties as QY8. `can-rx` is a
write-only QOM property whose value is a can-utils style hexadecimal
`ID#DATA` frame. QEMU offers the frame to configured RCAN receive mailboxes;
if none accepts it, the machine preserves the established SCPU `0x65` receive
path. This models an injected frame, not a CAN responder, and does not by
itself confirm that a guest driver accepted it.

For example, inject the `PUSH_SW`-shaped frame used by the T253 check:

```json
{"execute":"qom-set","arguments":{"path":"/machine","property":"can-rx","value":"280#400d81"}}
{"execute":"qom-set","arguments":{"path":"/machine","property":"can-rx","value":"280#400d01"}}
```

Set `can-log` and `can-replay` as machine properties at launch:

```sh
build-release/qemu-clarion --read-only --headless \\
    -M clarion-qy7,can-log=/tmp/qy7-can.log,can-replay=/tmp/can-replay.txt \\
    qy7-nor.bin map-card.img
```

`can-log` appends TX/RX frames and mode/filter changes. Frame records have the
same fields on QY7 and QY8: guest virtual milliseconds, direction, mailbox,
ID, format, DLC, and data. `can-replay` reads one periodic frame per line;
blank lines and lines beginning with `#` are ignored:

```text
# period in ms, then ID#DATA
100 280#400d81
```

The RCAN mailbox mechanism and interrupt behavior are unchanged. An injected
frame with no configured mailbox is logged with `mailbox=-1` before using the
existing SCPU receive path.

Dynamic QY7 replay confirmed that each tested subtype `36 86 60 xx yy`,
`36 83 3B FF xx`, and `36 86 6A xx yy` reaches the `lcddrv` key queue,
signals `EVENT_LCD_KEY`, and enters KEPDRV's `InputLcdKey` handler. KEPDRV
routes these to distinct state structures, but their physical meanings
(volume, tune/seek, or channel) are not established. A repeated key-down while
held is suppressed by the model; key-up still emits its release frame.

In the tested headless state, KEPDRV's `AuiKeyEvent` took its `PostMessageW`
path; its `keybd_event` call site was not reached. Statically, a nonzero
readiness flag at `ctx+0x64` selects `PostMessageW`; otherwise
`IsAPIReady(SH_WMGR)` is checked, and when that is false `IsWindowReady`
selects `keybd_event` for a ready window or `PostMessageW` otherwise. Display-micom
command `0x37` sends `key_code = 0xF0` with `KeyType = 1` in `kepdrv.dll`. On Nissan Leaf
(`Maker == 2`), the active key table `tbl_158` contains only MAP (`0x01`) and
Brightness (`0x02`), rejecting `0xF0` as `VK_KEY_NONE`. POWER on QY7 is routed through
MCAN as `AUDIO_ONOFF` (`20 0d`) instead.

The `PostMessageW` branch targets the HWND stored in KEPDRV's context and
posts `WM_KEYDOWN`/`WM_KEYUP` with the translated VK. The window's application
identity was not recovered from the available runtime state.

## 2DG window clipping

The 2DG model intersects a command's destination with the persistent window
clip registers (`CLIP_MIN`/`CLIP_MAX`) only when that command header carries
`M2DG_F_WCLIP` (`0x1000`). The system clip remains active for every command;
the one-shot `CLIPRECT` and `CLIPPATH` handling is independent of this flag.
This behavior is inferred from QY7 command-list diagnostics and is an emulator
model, not a hardware specification.

## Verified MCAN routes

These rows record guest-visible mappings from read-only QY7 captures. They do
not claim physical names for rows without a visible result. QY7 host keyboard
aliases send the five proven controls through MCAN (`PUSH_SW` for panel navigation keys,
and `AUDIO_ONOFF` function `0x20` for `POWER`); `D` and `B` use the display-micom frames above.
`M` is MAP on both series: on QY7 it sends the MCAN MAP/VOICE key, because
panel code `0x01` is day/night, not MAP.

| Panel control | Hard key | VKey | Verified guest result | Route |
|---|---:|---:|---|---|
| MAP/VOICE | `0x0C` | `0x0D` | Returns from menu/language screens to map | MCAN `PUSH_SW` |
| MENU | `0x44` | `0x1C` | Opens `MENY` from the map | MCAN `PUSH_SW` |
| STATUS | `0x31` | `0x1A` | Opens `Status / Lyd av / Ingen rute` | MCAN `PUSH_SW` |
| ZERO EMISSION | `0x4C` | `0x1F` | Opens the Zero Emission menu from the map | MCAN `PUSH_SW` |
| POWER | `0xF0` | `0x05` | Toggles media on/off (`AudioSourceSelect`, audio bar overlay) | MCAN `AUDIO_ONOFF` (`20 0d`) |

| Host key | Hard key | Panel control |
|---|---:|---|
| `N` | `0x44` | MENU |
| `M` | `0x0C` | MAP/VOICE |
| `T` | `0x31` | STATUS |
| `Z` | `0x4C` | ZERO EMISSION |
| `O` | `0xF0` | POWER |

Service menu: on the map, `M M M O O M` (MAP three times, POWER twice, MAP)
opens the system diagnostic menu (self diagnosis, confirmation/adjustment),
the documented way into Software Update on these units.

The MCAN keyboard input property is enabled only on QY7. Its first host key
starts the observed F-frame refresh and sends the observed `E0`/type-2 setup
before the press/release pair. Later pairs advance the three-bit message number
so both frames pass the session duplicate filter.

Other tested codes remain unnamed unless a visible result matched a control
on the panel photo. `0x12` and `0x47` remove a route overlay in the tested
state, but that does not identify a physical control. The Clarif30 evidence
records all tested VKeys and screenshots.

## Guest debug output

The `--dbgprint FILE` launcher option enables the bundled
`clarion-dbgprint` plugin. It prints calls to `NKDbgPrintfW`,
`NKvDbgPrintfW`, and `OutputDebugStringW` as `DBG <return-address> <text>`.
It reads SH-4 arguments from R4-R7 and the stack after the 16-byte home area.
The SH-4 GDB register XML exposes the active banked registers to the plugin
register API. The plugin scans every kernel XIP region in NOR and SD itself;
the SD card can contain multiple copies with different export addresses.

```sh
build-release/qemu-clarion --read-only --dbgprint guest-debug.log NOR SD
```

The launcher finds the plugin in the release package or build tree. XIP
exports have fixed virtual addresses, so no runtime module-load adjustment is
needed. For manual use, `-plugin PATH,image=NOR[,image=SD],out=FILE` performs
the same scan; `syms=FILE` remains available as a fallback for explicit
address lists.

## Debugging with a host debugger

Pausing the QEMU process with a host debugger while the guest is idle can let
the virtual clock catch up when execution resumes if `-icount` uses its
default `sleep=on`. On QY7 this can expire the modeled watchdog and make the
launcher print `power cycle; restarting QEMU`. This is a watchdog reset, not
the guest OFFP power-off path.

The QY7 watchdog is mapped at `0xFFCC0000` and uses `QEMU_CLOCK_VIRTUAL`.
Its default `wdt-pclk` is a provisional 24 MHz value, based on the documented
EXTAL frequency because the QY7 watchdog Pck has not been measured. With the
guest's observed watchdog settings this gives an estimated 4.38-second
timeout; the guest feeds it at about 3-second intervals.

In T290, five-second host-debugger stops with `wdt=on` caused a watchdog
power-cycle in 3/3 runs with the wrapper's `-icount shift=2` (the default
`sleep=on`). With `-icount shift=2,sleep=off`, 0/3 runs power-cycled. A
shorter one-second stop passed in one recorded control, so a short
stop is not a guarantee: the watchdog's phase matters. The `qy7_wdt_timeout`
trace event appeared before each observed restart in the five-second default
`icount` controls.

For host-debugger sessions, use:

```sh
build-release/qemu-clarion -machine clarion-qy7 -icount shift=2,sleep=off NOR SD
```

The equivalent machine property `-machine clarion-qy7,wdt=off` freezes the
watchdog, but changes guest runtime conditions; identify it in test reports.
The machine help lists `wdt` as a boolean, defaulting to on, and `wdt-pclk` as
a frequency in Hz. To check for a watchdog expiry, enable its trace event:

```sh
-trace enable=qy7_wdt_timeout,file=wdt.trace
```

With `sleep=off`, virtual time jumps to the next timer deadline as soon as
the guest CPU idles, without waiting for the corresponding wall-clock
interval. Guest timeouts may therefore elapse faster than real time during
idle periods; account for this when interpreting debugger runs.

## QY7 power-off and process restart

The micom frame `11 0d 00 01 00 00 40 20` is ordinary boot traffic and is
ACKed without a power-cycle action. The updater's YES path instead proceeds
through the kernel `OFFP` route. On X110, `nk.exe` writes zero to the board
power-hold register `0xB9100000` at guest PC `0x88047A24`; boot IPL first
asserts the same register with a write of one at `0xA0002488`. On X112, the
corresponding OFFP write is at `0x88047FB4`. This register transition models
the hardware releasing its power hold.

After the release, the QY7 machine requests shutdown with the shared
`CLARION_POWER_CYCLE_EXIT_CODE`. The `qemu-clarion` launcher treats that code
as a power cycle and starts a new child QEMU with the same arguments. A direct
`qemu-system-sh4` invocation exits with the power-cycle code because it has no
supervisor. With `--read-only`, NOR and SD qcow2 overlays stay in place across
the child restart and are removed when the launcher session exits; the NOR
overlay therefore retains guest flash writes such as clearing BMF.

## Diagnostics

QY7 model protocol traffic is quiet by default. Enable display-micom frames,
keys, retries, and acknowledgements with:

```sh
build-release/qemu-clarion --read-only --headless NOR SD \
    -trace 'clarion_dispmicom_*'
```

Other guest-driven model diagnostics using `LOG_GUEST_ERROR` are available
with `-d guest_errors`; this category can be verbose. Launcher messages, unit identification, and real
errors remain visible without these flags.

## HPB-DMAC partial RX sets with CT and DIP

QY7 SCIF RX uses two HPB-DMAC planes as one circular buffer. The guest's
`serial_scif.dll` consumer reads the ring through a pointer at
`0x03C8B0A8`, wrapping it with the `2 * size - 1` mask. Each plane is one
half of that ring; a short receive can end before the programmed transfer
count is exhausted.

At receiver idle, partial EOD signals that the received portion is available
and preserves the remaining transfer count in `DTCSR`. The guest consumes
that portion and writes `DNXT` to arm the other plane. `DNXT` does not discard
the unconsumed count or switch planes early: the active plane changes only
when `left == 0`. This gives SCIF3 touch-panel data and SCIF7 GPS data the same
two-plane ring behavior, without a channel-specific early-switch rule.
