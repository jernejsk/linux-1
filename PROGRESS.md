
## Transmit throughput: where the ceiling actually is

Transmit sits at 14.2 MB/s (13.1-15.5) on 5 GHz, SDR104 150 MHz, against 38 MB/s
of receive on the same link. Every host-side explanation was measured and ruled
out:

| Suspect | Measurement | Verdict |
| --- | --- | --- |
| CPU | 11% of four cores | not the limit |
| SDIO bus | ~20% occupancy | not the limit |
| Air time | receive does 38 MB/s, same AP and channel | not the limit |
| Short packets | tx_bytes/tx_packets = 1513 B | already full size |
| Small SDIO writes | `UWE5622_TX_MAX_SIZE` 16 -> 32 blocks: records per write 7.9 -> 14.4 | throughput unchanged |
| A-MSDU aggregation | ADDBA `amsdu_permit` bit set | no change: 13.1/14.8 MB/s, 1508 B per packet |

What is left is the controller's transmit credit scheme. Instrumentation over one
32 MB upload:

    tx_credits_granted=23457  tx_credits_used=23393  tx_credit_events=3850

One credit per packet, ~6.1 credits per flow-control event, ~9,760 credits per
second. 9,760 x 1500 B = 14.6 MB/s, which is the measured throughput to within
measurement noise. The host is never short of work or of bus; it is waiting for
the firmware to hand back permission to send.

Setting `amsdu_permit` in our own ADDBA request (we are the originator, so the
bit is ours to set) does not make the firmware pack several MSDUs into a frame,
so it cannot buy more bytes per credit. That change was reverted - it altered
negotiated block ack parameters for no measured gain.

Transmit is therefore firmware-limited, and maximised from the host side. The
remaining lever would be the credit-return path inside the CM4 image, which is
firmware work, not driver work.
