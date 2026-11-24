# Measuring Fleet timing

The device-neutral benchmark builds an installed Release SDK and exercises the
public C interface from a separate C++17 host. Keep build trees and results
outside the product checkout. Use a configured Release runtime build to select
the compiler and dependency inputs. Supply the frozen Single and Kohler CH750
JSON and impulse files in a local reference tree with their original paths.
The benchmark verifies their hashes before starting.

```sh
python3 tools/fleet_bench.py --profile-set r3-v1 \
  --source "$PWD" --build "$ES_OUT/runtime-test" --models "$ASSET_REFERENCE" \
  --seconds 600 --repeats 3 --output "$EVIDENCE/fleet-study"
```

The exact matrix uses 48 kHz with Interactive 128 and Balanced 256, static and
shared SDKs, and three fresh processes for each pairing of profile and workload.
Each process runs the Single in FREE and the CH750 in DRIVEN, with the Single
replaced every 60 seconds. The scheduled workload submits fixed-frame events
ahead of their targets. The loaded workload samples the host clock, schedules
against the current submission position, and adds deterministic work on the host
callback and on a third core. On Linux `--cores A,W,load[,C]` records the CPU IDs
for the callback, producer, background work and optional coordinator.
The host waits on its monotonic timer and spins for up to the final millisecond
or half a period, whichever is shorter. The spin belongs to the host timer and
its CPU time is included in the host report.

On Linux, add `--alsa-output INDEX 'EXACT DEVICE NAME'` to run the same
matrix against an explicitly selected native ALSA output through PortAudio
v19. Set `PKG_CONFIG_PATH` if the provider is installed outside the default
search paths. Use the PortAudio index and full device name, including its
`(hw:card,device)` suffix. The study sends the original mixed PCM to the
endpoint for 24 ten-minute runs and may be audible.

The physical host records actual callback sizes and flags, native ADC,
callback and estimated DAC times, and clock calibration brackets. It checks
completion against the earliest calibrated DAC timestamp, independently
counts xruns and verifies the same producer budget, command timing and stem
replay as the device-neutral host. The device clock and output queue estimates
cover the selected stream. Acoustic latency requires a separate loopback
measurement.

Add `--screen-only` to run 60-second rate, block-size and roster screens at
44.1 and 48 kHz. The variable-block screen repeats the maximum, one half of
the maximum and three quarters of the maximum. Roster screens use the Single,
CH750 and the frozen GM LS. Expansion within each roster family stops when
its next size fails, with the failing result retained.

The output captures the first callback, every absolute release and return,
producer seals and publications, accepted frames, receipts, and per-second
progress in both engine stems. It also records long producer idle polls and
any in-flight tile that never published. Voice snapshots report measured speed,
phase and physical targets. Their completed ticks and native frames are checked
against each model's clock. After a complete timed trial, an
unpaced consumer creates independent voices from the accepted timeline,
reconstructs the gain and replacement envelopes, and checks per-second and
full-stream stem digests. Consumer headroom is one quarter of each nominal
period. Producer headroom is evaluated over every consecutive 750 full tiles.
Live command measurements distinguish the sampling position, the producer's
publication and the callback's return. The measured callback interval excludes
the separate host workload. Deadline accounting includes it and counts late
wakes. A failure leaves a partial study and exits nonzero.

`FleetBenchmarkSelfCheck` uses the redistributable example models to exercise a
short dual-engine replay, including a replacement and a deliberately changed
gain control. It also rejects missed deadlines, underflow and a shifted marker.
`FleetLatencyProbe` compares a scheduled gain pulse and a
physical-control change against paired engine PCM. It reports transport lead,
first numerical latch, and the first above-threshold model response. Device
queue, DAC presentation, and physical loopback require their own measurements.
