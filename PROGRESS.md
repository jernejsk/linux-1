
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

## Access point mode, and what the firmware will and will not do (2026-08-13)

An access point above channel 14 could not start at all. The controller takes
the channel it beacons on from the beacon body and refuses one it cannot find a
channel in; the element that carries it, the DS parameter set, is defined for
2.4 GHz only, so hostapd never puts one in a 5 GHz beacon. `SET_CHANNEL` is
accepted and ignored for this purpose: with channel 36 accepted, `START_AP`
still answered `-EIO`, and dropping `ieee80211n` was enough to make it succeed,
which is what pointed at the beacon rather than the band. Supplying the element
when it is absent starts a 5 GHz access point at 20, 40 and 80 MHz;
`hostapd_cli status` reports `freq=5180 secondary_channel=1 vht_oper_chwidth=1`.
The vendor driver has the same requirement written into a debug helper that
rewrites DS parameter and HT operation channels, which is the hint that was
there all along.

5 GHz also needs a regulatory domain. Country 00 marks every 5 GHz range
passive-scan, so no access point may start there. `regulatory.db` was missing on
the board, and `iw reg set SI` silently did nothing until `iw reg reload` made
the database available; after that 5150-5250 is allowed at 80 MHz with no DFS
and the firmware accepts the domain.

Features added, all exercised on hardware: the stations an access point holds
can be listed (`dump_station`, built from the lookup entries the controller
reports); both access-control lists are wired to cfg80211's, with hostapd
reporting `nl80211: Set Accept ACL (num_mac_acl=2)` for an accept list and no
error for a deny list; the limits from `GET_INFO` are passed on, so hostapd now
sees `max_stations=10` where it saw zero; management frames can be registered,
delivered and sent, which is what silenced twelve `Register frame command
failed (type=208): ret=-95` per start; and a changed beacon body is handed over
with `RESET_BEACON` instead of only the elements the controller adds itself.

What the firmware refuses, tested one configuration at a time with the
interface present: protected management frames and WPA3-SAE. MFP fails when
hostapd installs the integrity key, because that key lives at index four and
the key path accepted only zero to three. Widening the range made things worse
rather than better: the firmware took the key and then died, with four
consecutive transmit failures, a failed CPU reset, a power cycle, and a module
unload left in D state needing the power switch. So the cipher advertisement and
the wider index were both reverted, and this stays a firmware limitation.
SAE has no path either: hostapd wants to handle authentication frames itself,
and those are answered by the controller. Open and hidden-SSID access points
both work, and three stop/start cycles in a row come up cleanly, so the wedge
was the key rather than the restart.

One thing accepted but not observed: no probe request was ever delivered, in
60 seconds on a busy 2.4 GHz channel or 18 on 5 GHz, although the registration
is accepted and hostapd reports `Enable Probe Request reporting`. Either the
controller does not report them in this mode or the subtype it wants differs
from the index the vendor passes. Action frames are untested for want of
something to exchange them with.

No client associated during this session, on either band; the laptop that was
meant to join never appeared, and the board has no second radio to scan with.
Association itself is not in doubt - an earlier session completed a WPA2 four-way
handshake and passed data both ways through this same 2.4 GHz path - so what is
untested here is throughput and the new station listing with a real client.

### What was actually killing the access point (2026-08-13, later)

Every access point session with a client died inside a minute or two, three
times over, each with the same signature: `4 consecutive transmit failures,
recovering`, a firmware CPU that would not go into reset, a power cycle, and an
access point that never came back while hostapd went on reporting it enabled.
Two separate causes, found by taking one thing away at a time.

**Registering management frame types.** The log said `command 0x16 timed out` -
command 22, `REGISTER_FRAME` - followed immediately by the transmit failures,
and removing registration removed the resets. That much is measurement; the
conclusion drawn from it, that registering a frame type is what stops the
controller, was too strong. Firmware analysis since (below) shows the handler
has no wait or loop and always queues a confirmation, so the command is not a
blocking operation. What the reverted code definitely got wrong is the payload:
it sent cfg80211's subtype index where the firmware wants the whole frame-control
value, so it asked for control frames instead of probe requests. Why the
controller then stopped answering is still unexplained; a dropped confirmation
inside the firmware is the leading candidate. Registration had been added earlier the
same day and never demonstrably worked: no probe request was ever delivered in
sixty seconds on a busy channel with the registration accepted. It is out again,
along with `mgmt_tx` and the `mgmt_stypes` declaration that invited it. After
that, an access point runs with no resets at all.

**Group traffic sent to an entry that is not there.** Every broadcast and
multicast frame went to lookup entry four, the vendor driver's fallback, whether
or not the controller had such an entry. Suppressing group frames alone kept a
client attached with no reset, which is what identified them; the entries the
controller does announce for the broadcast address (indices one and four) only
appear later and are torn down when the access point stops. Group frames are now
sent to a station instead, which is exactly right for a single-station access
point and keeps address resolution and address assignment working. Several
stations will need a copy each.

Also fixed: the controller takes a station's transmit lookup entry away without
always reporting that the station left, so hostapd was left holding a station it
could no longer reach, reporting it authenticated and associated long after its
client had gone. Losing the way to reach a station is now reported as the
station leaving.

Two things remain wrong. Unloading the driver while an access point interface
exists hangs `modprobe -r` in D state and needs the power switch - it happened
repeatedly and is the reason for several of today's power cycles. And after a
firmware reset the access point does not come back on its own: the driver does
call `cfg80211_stop_iface()`, but hostapd has to be restarted by hand.

The client used for all of this associated fine (WPA2, CCMP, `[AUTH][ASSOC]
[AUTHORIZED]`), took a DHCP lease from dnsmasq on the board, answered pings at
150-180 ms - its own power saving - and then dropped its interface after about a
minute each time. That last part is the client's network manager, not the access
point: the interface goes down on its side, which an access point cannot cause.
This link has no route to the internet, which is the usual reason a manager
gives up on a network; NAT would need `ip_tables`, which this kernel does not
build.

The access point's own address never changes: it is the permanent address with
the locally-administered bit set and bit 7 of the last octet flipped, so
`3c:7a:aa:31:6f:17` always becomes `3e:7a:aa:31:6f:97`.

### Why the attempted management registration could never work

RAM and ROM firmware analysis found that command `0x16` takes a packed
`{ __le16 frame_type; u8 register_frame; }`, where `frame_type` is the complete
IEEE 802.11 frame-control type/subtype (`0x0040` for probe request), exactly as
the vendor driver sends it. The reverted implementation sent cfg80211's
subtype index (`4` for probe request). Firmware decodes bits 2-3 as type and
bits 4-7 as subtype, so that request changed control/type 1, subtype 0; it did
not enable probe requests. This accounts for registrations apparently
succeeding while no probe request was ever reported.

The live RAM handler at `0x00143EB4`, the ROM handler at `0x0020A720`, and the
filter setter at `0x0022B19E` contain no wait or loop: after a bounded table
check/store, command 0x16 always queues a successful confirmation. The wrong
encoding is therefore proven, but it does not fully explain the observed
command timeout and controller failure. Authentication registration is also
explicitly ignored by the RAM handler. Keep registration, management TX, and
`mgmt_stypes` disabled. If this is ever investigated again, send one correctly
encoded raw command under DAP breakpoints before restoring any cfg80211-facing
capability.

One generic firmware failure can produce exactly such a timeout:
`cmd_send_cfm` (`0x002073A4`) obtains a descriptor with
`saved_msg_alloc(1)` (`0x0023273E`), but on pool exhaustion it only logs and
returns. It does not send an error or retry. Check that allocation's return
value under DAP if command 0x16 is ever probed again; this remains a plausible,
not demonstrated, cause of the lost confirmation.

### Teardown fixed, transmit stall still open (2026-08-13, later still)

Removing the driver with an access point interface present hung `modprobe -r`
in uninterruptible sleep and needed the power switch. Removal closes each
context and then unregisters the netdev, and unregistering an access point takes
cfg80211 through `stop_ap`, which then talked to the context just closed: one
command per timeout with the RTNL held. Returning from `stop_ap` when there is
no context to stop fixes it, and covers the reset case too. Verified: unloading
with a running access point now completes.

Protected management frames are now unreachable rather than merely unadvertised.
The BIP code point and its cipher translation are gone, so no cfg80211 path can
submit the key that wedged the firmware, and the two SHA256 key negotiations
that exist to be used with them are no longer offered. The index-above-three
rejection stays in both key paths, and its comment now records what was actually
observed rather than a guess about command layout.

Station reporting says what is known: authenticated and associated from the
controller's new-station event, the open port from the pairwise key that ends
authentication, and quality-of-service from the WMM element in the association
request. Protected management frames are never claimed. Read back from a live
client as `authorized/authenticated/associated/WMM: yes` with CCMP.

What is still wrong is the data path. With a client attached and traffic
running, SDIO writes start returning `-EBUSY` about a second apart, and after
four of them the driver resets the controller: `TX transfer failed: -16` x4 then
`4 consecutive transmit failures, recovering`. The best clean measurement before
a reset was 0.76 MB/s out and 0.46 MB/s in, against 31 MB/s for the same radio
as a station, and later attempts collapsed to a few tens of KB/s with the
transfer dying when the client went away. Transmit credits are the obvious place
to look next: if the access point context never gets credit returned, the driver
would push frames the controller will not take, which is what `-EBUSY` a second
apart looks like.

`dtim_period` is now 1 rather than hostapd's default 2.  It reduces multicast
and sleeping-client delivery latency, but the later client-power-save-off test
shows that it does not explain the remaining awake-client packet clustering.

Per-station link data is still missing - `signal: 0 dBm`, `tx bitrate: unknown` -
because the controller's station report is per interface and returns zeros in
access point mode.

### Firmware review of the 2.4 GHz AP rate (2026-08-14)

Combined RAM/ROM analysis found no fixed 12 Mbit/s AP rate cap.  It did find
that `host_cmd_start_ap` (`0x00209AC4`) forces every channel below 36 to internal
width zero (20 MHz), ignoring HT40 secondary-channel information.  The common
peer-width and rate-control paths consume that value, so 2.4 GHz HT40 is not
available in firmware AP mode.

The other built-in loss is A-MSDU: the vendor sends `amsdu_permit=0`, and
firmware overrides it only for a VHT peer on 5 GHz.  Two-point-four GHz still
has TX A-MPDU.  `blockack_alloc_tx_ba_session` (`0x00129BE4`) has no AP or band
exclusion; it requires an HT peer, a QoS-capable station slot and the global BA
permit.  Association processing marks the slot QoS-capable when it sees either
WMM or HT Capabilities, and the default A-MPDU buffer limit is 32.  Shared rate
control contains no AP maximum.  TKIP deliberately suppresses HT; CCMP does
not.

Therefore HT20 and missing A-MSDU reduce efficiency but do not explain 12
Mbit/s alone.  The vendor driver reaches the same roughly 12 Mbit/s with the
same firmware, client and channel.  This rules out a BA-install bug specific to
the new host driver: either firmware fails to use 2.4 GHz AP A-MPDU in both
cases, or aggregation is not the differentiator.

A Linux-client test with power saving disabled is also complete.  It gave 13.9
Mbit/s TCP transmit and 10/161/356 ms ping min/average/max, with replies still
clustered.  Client power saving is therefore not the primary throughput or
latency cause.

The decisive next test is to timestamp every ADDBA command/result and capture
the action exchange, A-MPDU flags, beacons and pings with an external monitor.
Result zero is emitted only after the peer accepts and firmware creates the
hardware A-MPDU LUT.  The firmware suspends that peer/TID queue while ADDBA is
pending and arms a 400-timer-unit timeout; both drivers retry a failed request
after three seconds.  Thus a failed negotiation can create one roughly 400 ms
hole per retry in both drivers.  It cannot explain a continuous 500 ms cadence.
Full firmware evidence and DAP addresses are in
`firmware-re/75-ap-throughput.md`.

The AP power-save path was checked too.  Firmware buffers only while the MH
hardware marks the peer asleep, and drains the per-AC queues when that state
clears; association-time U-APSD bits do not by themselves latch the peer asleep.
There is no fixed delivery cadence in the awake path.  Firmware TBTT processing
rebuilds the beacon, and its DTIM helpers gate the high/group power-save queue,
not normal awake-peer unicast.  With a 100-TU beacon and DTIM 1, the interval is
102.4 ms rather than 500 ms.  A monitor trace must establish phase alignment
before attributing the separate packet bunching to TBTT/DTIM.
