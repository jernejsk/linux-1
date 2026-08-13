
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

## Firmware logging off, trace ring gone

Channel 15 production is now disabled at the source, as
`firmware-re/50-debug-bt-misc.md` prescribes: `at+armlog=0\r\n` on the AT
channel (transmit 0, response 13) once the firmware answers, sent again after
every recovery because a reloaded firmware starts out logging. That byte clears
an enable tested before the expensive work in the emitters, so the firmware
stops formatting messages, stops building trace records and stops requesting
transport pages.

Measured over fourteen seconds of transmit at full rate:

| | records on channel 15 | transmit | CPU per MB/s |
| --- | --- | --- | --- |
| `firmware_log=1` | 1,830-1,842 | 32.6-33.0 MB/s | 4.45-4.48 |
| default (off) | **0** | 32.0-32.5 MB/s | 4.49-4.50 |

So the traffic really is gone - about 131 records a second, each an interrupt,
a bus read and an allocation - but it never was enough to measure against
22,000 packets a second of real traffic. The gains that are real: **half a
megabyte of ring per device**, 143 lines of driver, the debugfs interface, and
the firmware-side formatting work.

What stayed, deliberately:

- **Draining channel 15.** All logical channels share one FIFO, and a page
  already queued, a flush or an exceptional path can still deliver one. What
  arrives is counted in `log_records` and dropped; that counter is the only
  visibility left, and it reads zero in every test including across recovery.
- **Channel 14.** Fatal assertions go out there and must keep working.
- **A way back.** `firmware_log=1` keeps the log on, which is what the note
  asks of a host that wants an explicit capture.

Verified after a forced recovery: no AT timeout, `log_records` still zero, up
33.0 and down 36.8 MB/s.

## Recovery keeps the devices

Recovery used to delete both auxiliary devices, reload the firmware and build
them again. Measured cost of that: the interface came back as `wlan1`, then
`wlan2`, then `wlan3`. The name follows the **wiphy index**, iwd names the
interface `wlan<phy id>`, and the kernel never reuses a wiphy index while
cfg80211 stays loaded - confirmed three times in a row, phy0/phy1/phy2 against
wlan0/wlan1/wlan2. Nothing downstream of a destroyed wiphy can hold the name.

So recovery now keeps both devices and rebuilds only what the firmware forgot.
Clients were already told when the firmware goes away; there is now a second
callback for when it comes back, run once the core is carrying commands again:

- **Wi-Fi**: resend the version handshake and configuration, forget peers,
  block ack sessions and credits, reopen a firmware context for every interface
  that still exists, then report the link lost so userspace reconnects on the
  same interface. An access point interface is reported stopped to cfg80211.
- **Bluetooth**: inject a hardware error, which is what makes the stack reopen
  the device and rerun the setup - including the `0xfca1` dual-mode enable -
  that a fresh firmware needs. The old code did this *before* the reload, from
  the reset callback, where the stack's reopen could only be refused.

| | before | after |
| --- | --- | --- |
| interface after recovery | `wlan1`, `wlan2`, ... | **`wlan0`** every time |
| wiphy | new phy each time | **`phy0`** |
| Bluetooth | new `hci` index | **`hci0`** |
| reconnect | userspace re-discovers device | station reconnects itself |
| duration | ~2 s | ~2 s |

Tested: three consecutive recoveries, and one fired in the middle of a
transfer. Interface, phy and hci indices all stable, station reconnects by
itself, throughput after recovery unchanged (32.1 up, 38.7 down), no warnings
or faults, `log_records` still zero.

Not covered by these tests: the access point path, and whether Bluetooth
scanning still finds devices - there are no advertisers in range at this
location, so a scan returns nothing before recovery as well as after.

## Cleanup and the full regression pass

The driver had fourteen module parameters. Every one selected a path that
measurement had already rejected, or reported a number that an investigation
needed once. All fourteen are gone, along with the code behind them: hardware
roaming (Orange Pi issue 98 has no fix but not asking), the receive reorder
window, the single-record receive path, the 512-byte block size, the
wake-per-transfer sleep protocol, the poll-mode read loop, two unused frame
delivery modes, the firmware log switch, and the transmit credit, checksum and
receive timing counters. **The driver now has no module parameters at all**, and
is 920 lines shorter. Ethtool still offers the checksum offload, so nothing worth
controlling lost its control.

### Two bugs the cleanup exposed

**A parked controller was still being asked things.** A firmware parked for
system sleep answers nothing, so a key installation arriving just after the park
spent the whole command timeout failing: `command 0xe timed out`, three seconds
of the suspend path. Commands are now refused while parked. The first version of
that fix was worse than the bug - both edits landed in `suspend()` and nothing
cleared the flag, so after one sleep the flag refused *everything* and iwd
reported `Cannot send after transport endpoint shutdown` for every scan while the
station never reconnected. Resume now clears it first, before the interface
lookup, whether or not an interface is left to wake.

**Resume left the interface detached** if the wake command failed. It now
attaches either way: a stack that can time out and retry beats an interface
nothing can be sent through.

### Test matrix, all on the committed code

| test | result |
| --- | --- |
| cold boot | 5240 MHz, MCS 9, 80 MHz |
| transmit | 30.4-32.8 MB/s |
| receive | 36.3-41.4 MB/s |
| bidirectional | 21+21 to 28+28 MB/s |
| UDP 150 Mbit | 17.9 MB/s, no loss |
| 60 s soak | 34.0-34.2 MB/s, ~390 retransmits |
| suspend and resume, three cycles | stays associated on 5 GHz, no scan errors |
| driver reload | wlan0, phy0, hci0, full throughput |
| warm reboot | firmware ready without a power cycle |
| recovery | wlan0/phy0/hci0 kept, reconnects itself (validated before the test switch was removed) |
| Bluetooth | powered, dual mode, finds devices, no cost to Wi-Fi |
| oops, warnings, refcount errors | none, in any test |

Throughput and efficiency were then compared against the pre-cleanup build with
the two builds interleaved, because air conditions drift over an afternoon and a
sequential comparison measures the weather:

| | before | after |
| --- | --- | --- |
| transmit | 31.6, 30.9 MB/s | 32.8, 31.9 |
| receive | 39.5, 37.8 MB/s | 39.2, 36.3 |
| transmit CPU per MB/s | 4.55, 4.41 | 4.51, 4.61 |
| receive CPU per MB/s | 4.81, 4.86 | 4.85, 4.78 |

Transmit is slightly faster, everything else is inside ±2%. No regression.

### Measurement trap: the same SSID on both bands

Twice a cold boot measured 1-6 MB/s, and both times the cause was iwd
associating with the **2.4 GHz** BSS of a dual-band access point: `freq: 2422`,
`VHT-MCS 5`, 52 Mbit/s. Restarting iwd moved it to 5240 MHz and 30 MB/s. Two
hypotheses were tested and killed on the way - Bluetooth being powered or
scanning (33.8 MB/s during an LE scan) and power save (32.0 MB/s with it on).
Check the frequency before believing any number; the test suite now refuses to
measure below 5 GHz.

## WoWLAN: driver side done, physical wake still blocked

The trigger programming moved from `set_wakeup()` to `suspend(wiphy, wowlan)`,
where the configuration that applies to the coming sleep is handed over;
`set_wakeup()` now only enables the physical wake source. Wake-reason reporting
is implemented from the firmware's resume marker (common-header bit 3): the
first marked object of each sleep is kept, a marked event `0x81` reports
disconnect, a marked frame carrying the magic pattern reports a magic packet,
any other marked frame is reported as the packet itself, and nothing marked
reports an unknown cause. The report is made from a delayed work 500 ms after
resume, because the marked object cannot arrive before the transport reads
again, and `pattern_idx` is set to -1 so a disconnect does not also claim
packet pattern zero.

**Verified working:** with `any` armed over a sleep an ICMP echo arrived
during, the driver reported that exact frame (destination, source, protocol
all correct). With nothing arriving, it reported an unknown cause. So the
firmware really does mark objects built while asleep, and the host can recover
the reason from them.

**Not working: the controller never drives its host-wake pin.** All three
prerequisites from `firmware-re/30-boot-bind-sleep.md` are now implemented -
the single output the board wires (`bt-host-wake` -> sync bit 8 only), the
`AP_SUSPEND` transport handshake at function 0 register `0x1b0`, and the
three-step CP sleep request with its 65 us hold - and the driver now owns the
wake interrupt so it is armed *before* the controller is told the host is going
down, with any edge latched while masked discarded first. A firmware runtime
read at `0x00110248` confirms the configuration is exactly as intended:

    bt_en=1 wl_en=0 duration=2 (20 ms) levels=0/1 separation=0 irq_type=1
    data_len_max=0x690   transport state=0

The transport state stays **0** through the `AP_SUSPEND` write and for at least
100 ms after it, where the note says it should become 1. So the write is not
being honoured, and until it is nothing can pulse the pin. The register is a
write-1 strobe, so a read-back of zero says nothing either way.

Everything else was eliminated on the way: Bluetooth being powered or scanning
costs nothing and is not the wake source (its pulses turned out to be edges
latched while the interrupt was masked, replayed on unmasking); power save is
irrelevant; the magic filter is definitely armed, since ordinary traffic stops
being marked as soon as it is; and both a UDP-port-9 magic packet and a raw
ethertype 0x0842 frame fail to be matched, which is consistent with a filter
that passes nothing rather than one that misses the pattern.

Next step is the comparison the driver cannot make on its own: run the vendor
driver on this board and see whether WoWLAN wakes it. If it does, capture its
function 0 writes around suspend and diff them against the sequence above; if
it does not, the board's `BT-WAKE-AP` wiring or the firmware's pad routing is
the answer and `OrangePi_3_LTS_v1.4.pdf` decides it.

### WoWLAN, after wiring the Wi-Fi wake line (2026-08-13, later)

The board wires both outputs and the device tree now describes both, so the
configuration finally reads as intended: `bt=1 wl=1 split=1`, each radio driving
its own line, PM0 for Wi-Fi and PM1 for Bluetooth, both interrupts claimed
(`55` = GPIO 32, `56` = GPIO 33) and armed for the duration of a sleep.

Two facts were then established with instrumentation, and they narrow the
problem to one write:

- **The pulses seen on both lines are edges latched while the interrupts were
  masked, replayed the moment they are unmasked.** The handler logs
  `expected=0` for them, which is exactly when it should ignore them, so the
  earlier "PM0 pulsed" was not a magic packet after all.
  `irq_set_irqchip_state(IRQCHIP_STATE_PENDING, false)` does not clear them on
  this pin controller, so a genuine pulse cannot be told from a replay by count
  alone - only by the `expected` flag, which is what the driver now does.
- **The transport state at `0x00110248 + 0x1c` stays 0.** It should become 1
  once `AP_SUSPEND` is written. Writing `BIT(5)` to `0x1b0` as a function 0
  register (what the vendor documents) and as a function 1 register (what the
  vendor's own `sdio_func[FUNC_0]` indexing may actually mean) both leave it at
  0, with the write reporting success either way.

So everything the host controls is in place, and the one remaining question is
how to make that write reach the firmware's handler. Worth trying next, roughly
in order of cost: read `0x1b0` and its neighbours back through the CM4 debug
bridge rather than over SDIO, to see whether the byte lands anywhere; check
whether the CP needs an interrupt enable of its own (`REG_PUB_INT_EN0`, `0x1c0`)
before it will service `AP_INT_CP0`; and run the vendor driver on this board,
which settles in one test whether WoWLAN works here at all and, if it does,
gives a capture of the writes it makes around suspend to diff against ours.

### Forced transport state moves the blocker to the magic matcher

Forcing firmware runtime state `0x00110248 + 0x1c` to 1 read back correctly,
but a magic packet still produced no physical pulse and the board slept for the
full 101.9 seconds.  Both wake IRQ counts were only their old masked-edge
replays, consumed with `expected=false`.  Thus state 1 is necessary but not
sufficient: no accepted host-bound frame reaches `sdiom_tx_send`, so the
`AP_SUSPEND` delivery investigation is no longer on the critical path.

Static re-review found why the tested standard packets are rejected.  Firmware
`check_is_magic_pkt` at `0x0012D0FA` gets the six-byte repeated value from
`vif_bssid_get(vif)`, so it compares against the associated AP BSSID rather
than the station MAC.  In addition, `check_rx_magic_pkt` routes broadcast
destination frames around the matcher.  Both conventional test forms therefore
miss: broadcast packets never enter the matcher, while a unicast standard
packet repeats the wrong address from firmware's point of view.

The decisive next packet is unicast at Ethernet level to the station MAC, with
payload `ff` repeated six times followed by the AP BSSID repeated sixteen
times.  Keep transport state forced to 1 for this test.  If it pulses PM0 and
wakes, the firmware defect is proven end to end.  The direct repair is then a
single call-target change at RAM `0x0012D120`, from `vif_bssid_get`
(`0x00218A7C`) to signature-compatible `vif_mac_get` (`0x0021876A`), followed
by separate unicast and broadcast standard-magic validation.

The wake-IRQ ordering race is independently fixed: enable, synchronize the
replayed IRQ, set `wake_expected`, then issue `AP_SUSPEND`.  The relevant
filter and matcher functions were updated with these findings in the combined
Ghidra `wcnmodem.bin` program.

### WoWLAN works, and only the matcher was ever broken (2026-08-13)

Selective wake now works on hardware and the transport state does not need
forcing.  With trigger programming moved into `.suspend(wiphy, wowlan)`, a
unicast standard magic packet wakes the board from s2idle on the first attempt
and repeatably: ten consecutive cycles woke in 14.3-14.6 s of a 45 s sleep,
each reported to userspace as `magic packet received` with the frame attached,
each leaving the link on 5240 MHz, and 31.5 MB/s measured afterwards against a
30-32 MB/s baseline.  Everything earlier that read as "the firmware recognizes
nothing" was measured with no trigger armed for that sleep, because
`.set_wakeup()` is called only when wake-up as a whole is switched on or off.

The `AP_SUSPEND` handshake turned out not to matter.  The controller does not
act on function-0 `0x1b0` bits 5 and 6 at all: `AP_RESUME` does not clear a
transport state forced to 1, and `AP_SUSPEND` does not set it from 0.  The
handler exists (`0x00106E18` stores 1 to runtime `+0x1c`, `0x0010606E` stores 0)
but is never reached, because the byte it tests belongs to a status snapshot the
SDIO interrupt routine takes rather than to the written register.  Wake works
with the transport in state 0, so the forced-state diagnostic was a red herring
and has been removed.  Two earlier probes were artifacts: reading the SDIO slave
register block at `0x40140000` through the slave's own direct window returns all
zeros or all ones, and in firmware `0x40140000` is a data array base, not that
register block.  Function-0 `0x148` reads a constant `0x60` here.

The matcher defect is confirmed on hardware in both directions.  On stock
firmware a frame repeating the AP BSSID woke the board 3/3 while a standard
frame repeating the station MAC never did (2/2 full sleeps, zero pulses).  After
changing the call at `0x0012D120` to `vif_mac_get`, bytes `eb f0 ac fc` to
`eb f0 23 fb`, the results invert exactly: station MAC wakes 3/3, BSSID no
longer wakes.  A UDP magic packet addressed to the station's own IP wakes it;
the same payload broadcast cannot, because `check_rx_magic_pkt` routes broadcast
destinations around the matcher.  Since the repair lives in the firmware image,
this driver has to keep working with the stock behaviour, and the limitation
belongs in its documentation.

Command 83's encoding was read out of firmware rather than inferred.
`host_cmd_set_wowlan` (`0x00143D3E`) walks `{sub_cmd_id, pad_len, pad...}` TLVs
through a nine-entry table where subcommand 0 clears the flag word and
subcommand N ORs bit N.  The existing `{subtype, 0}` payload and the
reset-then-add sequence are correct as they stand.

Receive-path instrumentation settled wake reporting.  The controller marks its
own command responses while it considers itself asleep, so ids 248 and 5 arrive
marked on every suspend; only a marked disconnect event is a reason, which the
driver already required.  A magic wake does deliver its frame marked when the
packet is the ordinary UDP form, and that is what produces the exact reason.
For the case where nothing arrives marked, `uwe5622_woke_host()` now reports
whether the controller pulled the host out itself, which turns a wake of unknown
cause into a magic-packet wake whenever that was the only trigger armed.

Still untested: the disconnect trigger, which needs an access point that can be
made to deauthenticate the station on demand.  This host has no Wi-Fi interface
to inject from and the test AP is not under this session's control.
