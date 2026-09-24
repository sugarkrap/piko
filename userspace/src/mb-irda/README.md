# mb-irda

Matchbox tray applet for the infrared port. Clicking it opens a bubble with
three exclusive mode rows (Off / IrDA / Blaster) and a Discovery checkbox.

It owns no hardware. Every change is a line written to `ird` over
`/var/run/ird.sock`; state comes back from `ird` the same way, so the applet
never guesses and never persists anything itself.

A second connection stays subscribed to `ird` and joins the tray's select()
loop through `mb_tray_app_set_poll_fd()`, so the applet redraws when
something else changes the mode -- no polling.

## icon

`icons/mb-irda.png` is `pics/networksettings2/Devices/irda-large.png` from
Opie (`github.com/opieproject/opie`, GPL), the IrDA icon drawn for this same
class of machine. 32x32 RGBA, the size the tray scales from.

## build

Plain make, same rationale as mb-volume and mb-applet-card.

    make
    make DESTDIR=/tmp/irda-stage prefix=/usr install
