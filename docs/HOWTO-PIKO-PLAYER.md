# piko-player: an FLTK front-end for MPlayer

*Written 2026-08-03, when the ROM grew a graphical media player.*

`piko-player` is the ROM's video/audio player. It is **our** GUI — FLTK, on
the same Xfbdev every other client here uses — wrapped around MPlayer as the
decode engine. Companion to [`HOWTO-FLTK.md`](HOWTO-FLTK.md) (the toolkit it
is built with) and the MPlayer build in `tools/build-mplayer.sh`.

## Why not gmplayer

MPlayer ships its own GUI, `gmplayer` (`./configure --enable-gui`). It is a
**GTK+** program. This ROM has no GTK and no reason to grow one just for a
play button — the whole point of putting FLTK in the image was to write our
own GUI apps instead of dragging in someone else's toolkit stack. So the
engine stays MPlayer; the window, the buttons and the seek bar are ours.

## How it works

```
  ┌─ piko-player (FLTK) ────────────┐
  │  ┌───────────────────────────┐  │   child X window (VideoBox),
  │  │  MPlayer -vo x11 -wid …    │  │ ← its X id handed to MPlayer
  │  │      draws video here      │  │   so MPlayer draws INTO it
  │  └───────────────────────────┘  │
  │  [Play] [Stop] [Open] ▁▁●▁▁ 0:12 │   FLTK controls
  │  Volume ▁▁▁●▁▁                   │
  └─────────────────────────────────┘
        │ stdin  (commands)   ▲ stdout (ANS_* replies)
        ▼                     │
     one long-lived MPlayer, -slave -idle
```

Two ideas do all the work:

1. **`-wid` embedding.** piko-player's video area is a real child X window
   (a nested `Fl_Window`). We pass its X id to MPlayer with `-wid`, and
   `-vo x11` draws the video straight into it. FLTK never touches a video
   pixel; MPlayer never draws a control. (This device has no Xv, so `-vo x11`
   with `-zoom` for scaling is the video output — not `-vo xv`.)

2. **Slave mode.** MPlayer is started once, `-slave -idle`, and stays alive
   for the whole session. piko-player writes one-line commands to its stdin
   (`loadfile`, `pause`, `stop`, `seek`, `volume`) and reads playback
   position back off its stdout (`ANS_TIME_POSITION=`, `ANS_LENGTH=`), polled
   twice a second while something is playing.

Source: `userspace/src/piko-player.cxx`. Deliberately small — open, play,
pause, stop, seek, volume. No playlist, no fullscreen, no equalizer: this is
a 400 MHz PXA255, and a core player that is solid beats a feature list that
stutters.

## The MPlayer it needs

An **X11-enabled** MPlayer. The old build was framebuffer-only and fully
static; `-wid` is a no-op there. `tools/build-mplayer.sh` now builds `-vo
x11` (keeping `-vo fbdev` too) against the X11/Matchbox stack's `libX11`
(staged in `userspace/stage-target`), so the binary is dynamic and NEEDs
`libX11.so.6` + its helpers — all already in the ROM's `/lib`. The build
script fails loudly if that library did not link, precisely so it cannot
quietly regress to a framebuffer-only binary the GUI can't drive.

**Where MPlayer lives: the SD card, not the ROM.** MPlayer with its bundled
ffmpeg is far too big for the ~68 MiB NAND root, so — exactly as before — the
engine ships to `/mnt/card/.zaurus/usr/bin/mplayer` (the manifest's
"card-only destination"), while only the small FLTK GUI ships in the ROM.
piko-player finds MPlayer by looking, in order: `$PIKO_PLAYER_MPLAYER`,
`/usr/bin`, `/usr/local/bin`, the card path, then `$PATH`. With no card in,
it says so in a dialog instead of being a dead icon.

> If you ever decide MPlayer *should* live in the ROM (budget permitting),
> add its binary to `tools/build-matchbox-payload.sh`'s `BINS` at
> `usr/bin/mplayer` — piko-player already looks there first, so no code
> change is needed. Its `DT_NEEDED` (libX11/libXext/libxcb/libXau/libXdmcp +
> libc) is already satisfied by the payload's `LIBS`, so the dependency check
> passes as-is.

## Build and ship

```
tools/build-x11-stack.sh          # stages libX11 etc. into stage-target
tools/build-fltk.sh               # builds piko-player into stage-target/usr/bin
tools/build-alsa.sh               # libasound.a for MPlayer
tools/build-mplayer.sh            # the X11 MPlayer (-vo x11 + -vo fbdev)
tools/build-matchbox-payload.sh --deploy --adapter wlan0 root@<ip>
```

`build-fltk.sh` compiles `piko-player.cxx` the same way it does
`piko-settings` — plain `-lfltk` (it decodes no images of its own, so no
`libfltk_images`) — and asserts the result NEEDs `libfltk.so.1.3`.
`build-matchbox-payload.sh` ships the binary to `/usr/local/bin/piko-player`
and its `.desktop` + icon (`userspace/desktop/piko-player.{desktop,png}`) so
it appears on the desktop.

## Try it on the device

```
DISPLAY=:0 piko-player            # from a shell over SSH, or tap the icon
```

Then **Open** a file (the chooser starts on `/mnt/card`, where media lives).
`PIKO_PLAYER_MPLAYER=/path/to/mplayer piko-player` points it at an alternate
build.

## Known limits / where to look if it misbehaves

- **Nothing but a black box, no video.** Almost always the MPlayer end:
  confirm the binary is the X11 one (`readelf -d $(which mplayer) | grep
  X11`) and that it plays the file standalone with `-vo x11`. piko-player
  only forwards commands.
- **Playback is slow / tears.** Expected headroom problem: software YUV→RGB
  plus X on a 400 MHz part. `-zoom` scaling costs the most; small clips at
  native size are kindest.
- **Seek bar does not move.** piko-player learns length/position from
  MPlayer's `ANS_*` lines; if MPlayer was built `-really-quiet` those never
  print. The build here uses `-quiet`, which keeps them.
