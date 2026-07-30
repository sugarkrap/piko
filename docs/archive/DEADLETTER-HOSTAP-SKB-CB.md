# Dead Letter — hostap "invalid skb->cb magic": card associates but passes no data

*Written 2026-07-23. The WiFi-only-access constraint (`AGENTS.md`) makes this
load-bearing: no USB, no serial, so hostap→SSH is the only remote path.*

---

## Symptom

Prism2 PCMCIA card on the mainline `hostap` driver: scans fine (radio OK),
**associates** to the AP (`iwconfig` shows the BSSID), but **100% packet
loss** — no ARP, no ping. `dmesg` shows, repeatedly:

```
wifi0: LinkStatus=1 (Connected)   BSSID=fe:ea:f1:74:dd:42
wifi0: invalid skb->cb magic (0xf08a0001, expected 0xf08a36a2)   (x N)
wifi0: LinkStatus=2 (Disconnected)
wifi0: LinkStatus=6 (Association failed)
```

The link flaps (connect → drop → reassociate) because *every* data frame is
dropped. Cacko (2.4 kernel, same card, same AP) worked fine — the tell that
it's a mainline-driver regression, not hardware. (Following the standing
"always check how Cacko did it" rule pointed straight at the driver.)

## Cause

hostap uses a **master radio device (`wifi0`)** plus per-role virtual
devices (`wlan0` data, `wlanXap` AP). The data device's `ndo_start_xmit`
(`hostap_data_start_xmit` / `prism2_sta_send_mgmt`) stashes hostap TX
metadata — a **magic (`HOSTAP_SKB_TX_DATA_MAGIC = 0xf08a36a2`) plus the
source `iface` pointer** — into **`skb->cb`**, then forwards the frame to
the master with `dev_queue_xmit(skb)`. `hostap_master_start_xmit` later
re-reads that magic from `skb->cb` to recover the iface.

On modern kernels, `dev_queue_xmit` runs the destination's **qdisc**, and
the qdisc layer uses `skb->cb` for its own `struct qdisc_skb_cb` — which
**overwrites hostap's magic** (observed: low 16 bits `36a2`→`0001`, high
half intact). The master then sees a corrupt magic and drops the frame.
hostap was written for 2.4-era kernels where `dev_queue_xmit` did not touch
`cb`.

The struct `hostap_skb_tx_data` even starts with an
`__padding_for_default_qdiscs` field (whose comment admits it's "A HORRIBLE
HACK THAT SHOULD NOT LIVE TO SEE THE DAY") specifically to keep the qdisc's
`pkt_len` write off the magic. But it was only **4 bytes** — sized for an
older, smaller `qdisc_skb_cb`. This kernel's `qdisc_skb_cb` is:
```c
unsigned int pkt_len;   /* cb[0..3] */
u16          pkt_segs;  /* cb[4..5]  <-- lands on magic's low half */
u16          tc_classid;/* cb[6..7] */
```
and `qdisc_pkt_len_init()` writes **both `pkt_len` and `pkt_segs`** on every
`dev_queue_xmit`. So `pkt_segs = 1` overwrote `magic` (offset 4) → `0x...0001`.

## Fix

> **2026-07-30, second revision:** this was originally written as a two-part
> fix. **Part 2 (`IFF_NO_QUEUE`) was wrong and has been reverted** — it caused
> a second, separate failure. Only part 1 is the fix. See
> "Part 2 was wrong" below before re-applying anything from this document.

**1. Enlarge the padding** so `magic` sits past the whole fixed
`qdisc_skb_cb` head (`hostap_wlan.h`):
```c
unsigned int __padding_for_default_qdiscs[2];  /* 8 bytes, was 4 */
u32 magic;                                      /* now at cb[8] */
```
This is the load-bearing fix — it directly protects the magic from the
`pkt_len`+`pkt_segs` write that `qdisc_pkt_len_init()` does on *every*
`dev_queue_xmit`, regardless of qdisc.

This is the whole fix. Struct stays well under the 48-byte `skb->cb` limit
(~28 bytes). Rebuild the `hostap` module, reinstall to
`nand-root/lib/modules/7.1.4/`, flash mtd3. The patched sources live in the
**tracked** `modules/hostap/` (kernel-src is gitignored and gets rebuilt from
it by `tools/setup-kernel-src.sh` — that is how the first version of this fix
was silently lost).

## Part 2 was wrong: `IFF_NO_QUEUE` on the master (reverted 2026-07-30)

The original fix also set `dev->priv_flags |= IFF_NO_QUEUE` on the master, on
the theory that `magic` now lives in the qdisc *private `data[]`* region
(cb[8..27]) which a real qdisc's enqueue could write, so removing the qdisc
protects it.

**Do not re-apply this.** It fixed nothing and broke the link a second way —
this is what the "our WiFi is flaky compared to Cacko" report was.

- **It was never needed.** The concern is real in general (`fq_codel` writes
  `data[0..7]`), but this kernel is built with `CONFIG_NET_SCHED` unset, so
  the only qdiscs in existence are the built-in `pfifo_fast` and `noqueue`,
  and `pfifo_fast_enqueue()` never touches `data[]`. The padding alone is
  airtight here.
- **It actively breaks TX.** `prism2_transmit()` (`hostap_hw.c`) calls
  `netif_stop_queue(local->dev)` on **every** frame, from inside the master's
  own `ndo_start_xmit`, and only reopens the queue later from the hardware
  IRQ (`prism2_transmit_cb`, `prism2_alloc_ev`). The next packet is supposed
  to wait in the master's qdisc — that is precisely why the AP and data
  interfaces *are* `IFF_NO_QUEUE`, with the upstream comment "use main radio
  device queue". Take the master's queue away and `__dev_queue_xmit()` sees
  `netif_xmit_stopped(txq)`, skips the transmit, and frees the skb via
  `net_crit_ratelimited("Virtual device %s asks to queue packet!")`
  (`net/core/dev.c`).
- **The loss is invisible in the counters.** `hostap_data_start_xmit()` does
  `dev_queue_xmit(skb); return NETDEV_TX_OK;` — the return value is
  discarded, so `wlan0`'s `tx_packets` climbs for packets that were dropped.
  On an 802.11b card the stopped window is ~1 ms, so a 1 Hz ping never
  collides with it and looks perfect, while ssh/scp/DHCP/ARP+reply bleed
  packets continuously.

**On-device check:** `dmesg | grep 'asks to queue'`. Any hit means the master
is noqueue and this regression is back.

If `CONFIG_NET_SCHED` is ever enabled, pin the master to a cb-safe qdisc
(`tc qdisc replace dev wifi0 root pfifo_fast`) — do not reach for
`IFF_NO_QUEUE`.

> Historical note: `IFF_NO_QUEUE` alone was also never sufficient for the
> original bug — `qdisc_pkt_len_init()` writes `cb[0..7]` *before* the
> noqueue check in `__dev_queue_xmit`, so the first attempt (noqueue only)
> still had the magic clobbered. It was wrong in both directions.

## Related config that had to be right first

- **`iw_mode=2`** (`IW_MODE_INFRA`) — hostap defaults the card to Master/AP
  mode; the runtime Master→Managed switch misbehaves. Pre-load `hostap` with
  `iw_mode=2` (in `rcS` before the PCMCIA host modules, and in
  `/etc/modprobe.d/hostap.conf`) so the card comes up as a client cleanly.
- **`key off` / `power off`** in the bring-up (`wifiup` / `wifi-up.sh`) —
  clears a stale WEP privacy bit and disables power-save (which drops
  broadcast/ARP on these old cards).
- **`CONFIG_PACKET=y`** in the stage-2 kernel — `udhcpc`'s raw `AF_PACKET`
  socket needs it (though DHCP itself is unreliable on this driver; we use
  a static IP — see `wifi-up.sh`).

## Standing note
Do NOT suggest USB/serial as an alternative — they do not exist for this
device (`AGENTS.md`). WiFi is the only path; fixes like this are mandatory.
