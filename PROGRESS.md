
## Transmit throughput

**The earlier "transmit is credit-limited at 14.6 MB/s" conclusion in this file
was an artefact of the measurement, not a property of the device.** Uploads were
measured with `curl -T` against a Python `http.server`, and that receiver caps a
single connection at about 15 MB/s. Against `iperf3` over the same link, same
driver, same firmware, transmit does **30 MB/s**.

The credit arithmetic that appeared to confirm the 14.6 MB/s ceiling was
circular. One credit is spent per packet, so credits per second multiplied by
packet size always reproduces the measured throughput, whatever the throughput
happens to be. It cannot distinguish a credit limit from any other limit.

Real numbers, 5 GHz VHT80 one stream, MCS 9, SDR104 at 150 MHz:

| | throughput | CPU (4 cores) |
| --- | --- | --- |
| upload, 1 stream | 30.6 MB/s | 19% |
| upload, 4 streams | 31.7 MB/s | |
| download, 1 stream | 40.5 MB/s | |
| bidirectional | 20.3 up + 20.0 down | |

One stream reaches within 4% of four streams, so nothing per-flow is throttling
transmission.

### What the transmit rate actually depends on

How much the driver packs into one CMD53, measured over ten seconds each:

| blocks | throughput | queue stopped |
| --- | --- | --- |
| 8 | 22.7 MB/s | 1.3% |
| 16 | 26.6 MB/s | 2.0% |
| 32 | 30.9 MB/s | 5.8% |
| 48 | 31.9 MB/s | 9.6% |
| 64 | 31.7 MB/s | 12.1% |

The knee is at 48 blocks, now the default. This is the same experiment that
looked like it changed nothing when the Python receiver was setting the pace.

### Ruled out, with numbers

- **Per-flow queue limits.** `sk_pacing_shift` 10, 8, 7 and 6 all give 30-32
  MB/s. TCP small queues are not involved, so the mac80211 pacing-shift trick
  buys nothing here and was dropped.
- **Reordering.** `ss -ti` shows `reord_seen:566` on a loaded flow, but
  `tcp_reordering` 3 versus 127 changes nothing (15.6 versus 15.4 MB/s on the
  old harness), and retransmits at full speed are 30 of 21,000 segments.
- **A-MSDU.** Setting `amsdu_permit` in our own ADDBA request changed nothing,
  and `firmware-re/65-tx-flow-control.md` explains why: the firmware sets bit 0
  itself in `blockack_alloc_tx_ba_session` (`0x00129BE4`) whenever the peer is
  HT-capable on 5 GHz, so the host value is redundant on this link, which is also
  why the vendor writes zero. Reverted as redundant, not as ignored. Note that
  `tx_bytes / tx_packets` staying at 1508 B proves nothing either way: those are
  host counters over the original Ethernet frames, and a transmit A-MSDU is
  assembled past them in the MAC. Either way it cannot raise throughput here,
  because every constituent MSDU has already spent its own credit before the
  hardware can combine them.
- **Pinning a flow to one credit pool.** Do not do this: the controller
  replenishes the pools it chooses, not the one being drained. Colour 0 hit zero
  while 1, 2 and 3 sat at 19, and since the queue is only woken by a grant,
  transmit deadlocked. The fallback to unclaimed pools is load-bearing.

### Where the remaining gap is

Transmit 30 MB/s against receive 40 MB/s on a 54 MB/s PHY. The queue is stopped
9.6% of the time at 48 blocks, and grants arrive in lumps of 9 to 16 credits
because the firmware reposts buffers only once eight have accumulated
(`0x0013A968`, see `firmware-re/65-tx-flow-control.md`). Lowering that threshold
is worth at most the stopped fraction, and only if the MAC can drain while the
host waits. The rest of the gap is air-side asymmetry between what this radio
transmits and what the access point transmits.

### Block ack negotiation result

The firmware answers `WIFI_CMD_ADDBA_REQ` with thirteen bytes: the negotiation
result, then the request echoed back. The worker discarded it, so a declined or
timed-out session was indistinguishable from an accepted one, the TID kept its
bit, and that traffic stayed unaggregated for the life of the association. Now
parsed, with the same clear-and-back-off treatment a failed command already got.
Verified live: `ret=0 len=13 result=0` from an access point that accepts.

The echo is not evidence of A-MSDU acceptance. The firmware saves the request
before adding its own bits to the frame it sends, so proving A-MSDU use needs a
monitor capture or instrumentation at `machw_create_amsdu_lut` (`0x001534FE`).

### Measurement rule

Never measure a link with a userspace sink in the path. Use `iperf3`, and bind
it to the interface with `-B <addr>%<dev>` - binding the address alone routes
over Ethernet and reports 110 MB/s of gigabit link.

## Transmit efficiency

Two offloads and one copy removal, measured on 5 GHz VHT80 against the same
access point, one stream:

| | throughput | CPU of four cores | CPU per MB/s |
| --- | --- | --- | --- |
| before | 30.6 MB/s | 19.0% | 5.9 |
| hardware checksum | 31.4 MB/s | 18.2% | 5.8 |
| one copy instead of three | 32.9 MB/s | 14.9% | 4.5 |

**Hardware transmit checksum.** `desc[2]` bit 0 enables it, bit 1 selects TCP
over UDP, `desc[9..10]` carry the transport offset from the Ethernet header.
Verified on air with the software path taking nothing: IPv4 TCP 317,008 frames,
IPv4 UDP 103,617 datagrams with no loss, IPv6 TCP 190,486 frames. Worth about
0.3% of four cores at a rate held equal, which is small enough that the driver
counts what each path handled.

**One copy instead of three.** The frame was copied into a descriptor buffer,
then into a bus record, then into the transfer. Now the netdev asks for
headroom and both headers are pushed in front of the frame where it lies. This
required flipping the documented bus contract: the bus owns a payload it
accepts and frees it after the transfer.

### The remaining copy stays

Packing several frames into one CMD53 still copies. Removing it would mean
driving `mmc_request` with a scatterlist instead of `sdio_writesb`, and that
does not fit this hardware: after the 15 bytes of headers are pushed, a frame
starts at `NET_SKB_PAD - 15`, which is not word aligned, while the sunxi mmc
controller's DMA wants aligned segments — and the four-byte padding between
records breaks contiguity anyway. The upside does not justify it either: one
memcpy of the traffic at 33 MB/s against roughly 1.75 GB/s of memory bandwidth
is about 2% of one core, half a point of the 14.9% now measured.

### Two teardown bugs found on the way

Removing the driver with an interface up warned twice, and both were real:
a netdev freed with its NAPI instance still attached (the driver's own remove
path never stopped the poll, emptied its queue, or cancelled two works a
firmware event can schedule), and cfg80211 still holding the BSS of a station
that never saw a disconnect. Every teardown path now goes through one function.

### Trap worth remembering

Deploying a new `wifi.ko` against an old `core.ko` corrupts the skb slab and
panics the board at every boot thereafter, because the two disagree about who
frees a transmitted frame. Recovery needs U-Boot: interrupt autoboot, then

    setenv bootargs "console=ttyS0,115200 root=/dev/mmcblk0p1 rw rootwait init=/bin/sh"
    load mmc 0:1 ${kernel_addr_r} /boot/Image
    load mmc 0:1 ${fdt_addr_r} /boot/sun50i-h6-orangepi-3-lts.dtb
    booti ${kernel_addr_r} - ${fdt_addr_r}

and move the module directory aside from that shell. Deploy both modules
together, and check the deployed hash: a scp that times out leaves the old one
in place and the next boot panics on it.
