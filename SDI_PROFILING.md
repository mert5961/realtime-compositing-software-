# SDI profiling findings

Hardware: Corvid44-12G-8K, AJA driver 18.0.0.1063, SDK 18.0.0.
Signal: 1080p50a camera on SDI 1, television on SDI 4, video only.
The SDK selects frame store 3 for playout and routes it to SDI 4. A physical
connector number is not necessarily the frame store handling AutoCirculate.

## Findings and corrections

The original smaller-buffer drops had two code-level causes:

1. The SDK example subscribed to output vertical interrupts on frame store 1,
   then waited on its actual output frame store, 3. In the profiled 3/3 and 4/4
   runs, every output device-space wait returned failure, averaging roughly 55–56 ms.
   At 50 FPS, that spans nearly three frame periods. Output delivery stalled,
   the host FIFO filled to ten entries, and capture was blocked waiting for a
   reusable entry. We now enable and subscribe to the actual output frame
   store, check both return values, and unsubscribe at teardown.
2. After the interrupt fix, 4/4 and 7/7 were clean, but 3/3 still dropped
   regularly. A 3-input/7-output test recorded 166/163 drops in 10.031 s,
   while a 7-input/3-output test recorded zero drops in 10.014 s. The SDK's
   capture predicate waits for two buffered frames to protect the audio write
   head. This application disables audio. Its video-only capture now accepts
   one completed frame; the two-frame predicate remains for audio-enabled
   operation. The corrected 3/3 test subsequently reported zero drops.

DMA API calls measured around 1.5 ms for capture and 1.7 ms for output;
capture processing/timecode took about 0.02 ms. Following both corrections,
capture host-frame acquisition took about 0.001 ms. With three/four card
buffers, output host-frame acquisition was also about 0.001 ms in steady
operation. With seven, output can instead wait about 18 ms on the host for
the next camera frame; that is expected pacing, not a host FIFO blockage.
Input/output device waits near 18 ms are normal pacing of a 20 ms frame period, with DMA
and processing using the remainder. The run data below provides exact values.

The SDK startup prefill is also adjusted for a three-slot output ring: it
queues two frames before starting, because its acceptance rule reserves one
slot. Larger rings retain the three-frame prefill.

## Measurements and limitations

- The UI displays cumulative average/max time and API failure counts for DMA,
  host-frame acquisition, device waits, and capture processing, plus current
  host ownership stages and peak occupancy.
- Per-thread trace memory is allocated ahead of capture. Every recorded event
  has a monotonic completion timestamp, duration, success flag and host-stage
  snapshot. Frame loops perform no trace-file writes or memory allocations.
- Timing is synchronous API wall time, including driver and scheduler effects.
  These are not hardware-only DMA measurements or Windows scheduling traces.
  Host snapshots around concurrent stage transitions are approximate.
- Each thread records at most 65,536 events. Aggregates continue once a trace
  fills, and the saved summary reports discarded events. Shutdown events are
  marked and excluded from aggregate metrics. The comparison analyzer also
  excludes the first second for its timing statistics; drop totals include it.
- These tests are limited to this camera/card/format and test duration. They
  do not measure camera-to-TV latency or tracking synchronization.

Four device buffers per direction are the current default. The host FIFO
capacity remains ten; reducing host allocation is a separate change.

## Recorded comparisons

| Implementation | Buffers | Duration | Capture drops | Output drops | Host peak |
|---|---|---:|---:|---:|---:|
| Before fixes | 3/3 | 30.027 s | 540 | 549 | 10/10 |
| Output interrupt fix only | 3/3 | 30.217 s | 503 | 502 | 2/10 |
| Both fixes | 3/3 | 30.178 s | 0 | 0 | 3/10 |
| Both fixes | 4/4 | 60.103 s | 0 | 0 | 2/10 |
| Both fixes | 7/7 | 30.245 s | 0 | 0 | 3/10 |

Detailed analyzer output is appended below. Raw sample, trace and summary
CSV files are retained in the ignored `output` directory under matching run IDs.

## Before corrections

### sdi-17918946666290263-trace

Card buffers 3/3; duration 30.027 s; capture/output drops 540/549; host peak 10/10; discarded trace events 0.

Timing statistics exclude the first 1 second(s) and shutdown. Counts and drop totals above include startup.

| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |
|---|---:|---:|---:|---:|---:|---:|
| input_device_wait | 1384 | 16.051 | 19.995 | 20.397 | 47 | 0 |
| capture_host_wait | 926 | 5.764 | 44.399 | 61.554 | 74 | 0 |
| capture_dma | 926 | 1.511 | 1.642 | 2.894 | 0 | 0 |
| capture_processing | 926 | 0.018 | 0.023 | 0.030 | 0 | 0 |
| output_device_wait | 493 | 55.510 | 62.515 | 65.749 | 493 | 493 |
| output_host_wait | 918 | 0.001 | 0.002 | 0.007 | 0 | 0 |
| output_dma | 918 | 1.730 | 1.963 | 3.403 | 0 | 0 |

Host occupancy range in recorded operation-boundary snapshots:
- host_capture: 0–1
- host_ready: 1–10
- host_output: 0–1
- host_occupied: 1–10

DMA measurements are synchronous API elapsed times, including driver/scheduler overhead. Wait measurements include normal pacing and event/lock delays. These are not hardware-only DMA timings or OS scheduling traces. Occupancy snapshots near stage transitions are approximate and event-weighted.

### sdi-17918946968065972-trace

Card buffers 4/4; duration 30.009 s; capture/output drops 244/252; host peak 10/10; discarded trace events 0.

Timing statistics exclude the first 1 second(s) and shutdown. Counts and drop totals above include startup.

| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |
|---|---:|---:|---:|---:|---:|---:|
| input_device_wait | 1410 | 13.023 | 19.992 | 20.421 | 29 | 0 |
| capture_host_wait | 1206 | 7.211 | 33.439 | 43.576 | 205 | 0 |
| capture_dma | 1206 | 1.539 | 1.672 | 2.153 | 0 | 0 |
| capture_processing | 1206 | 0.018 | 0.024 | 0.061 | 0 | 0 |
| output_device_wait | 478 | 56.110 | 62.493 | 65.865 | 478 | 478 |
| output_host_wait | 1205 | 0.001 | 0.001 | 0.004 | 0 | 0 |
| output_dma | 1205 | 1.756 | 2.010 | 3.368 | 0 | 0 |

Host occupancy range in recorded operation-boundary snapshots:
- host_capture: 0–1
- host_ready: 7–10
- host_output: 0–1
- host_occupied: 8–10

DMA measurements are synchronous API elapsed times, including driver/scheduler overhead. Wait measurements include normal pacing and event/lock delays. These are not hardware-only DMA timings or OS scheduling traces. Occupancy snapshots near stage transitions are approximate and event-weighted.

### sdi-17918947269638202-trace

Card buffers 7/7; duration 30.035 s; capture/output drops 0/0; host peak 3/10; discarded trace events 0.

Timing statistics exclude the first 1 second(s) and shutdown. Counts and drop totals above include startup.

| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |
|---|---:|---:|---:|---:|---:|---:|
| input_device_wait | 1452 | 18.466 | 18.503 | 18.539 | 0 | 0 |
| capture_host_wait | 1452 | 0.001 | 0.002 | 0.004 | 0 | 0 |
| capture_dma | 1452 | 1.465 | 1.516 | 1.591 | 0 | 0 |
| capture_processing | 1452 | 0.019 | 0.024 | 0.048 | 0 | 0 |
| output_host_wait | 1452 | 18.324 | 18.396 | 18.485 | 0 | 0 |
| output_dma | 1452 | 1.654 | 1.706 | 1.755 | 0 | 0 |

Host occupancy range in recorded operation-boundary snapshots:
- host_capture: 0–1
- host_ready: 0–1
- host_output: 0–1
- host_occupied: 0–1

DMA measurements are synchronous API elapsed times, including driver/scheduler overhead. Wait measurements include normal pacing and event/lock delays. These are not hardware-only DMA timings or OS scheduling traces. Occupancy snapshots near stage transitions are approximate and event-weighted.

## After both corrections

### sdi-17918950897742420-trace

SDI 1 → 4; frame stores 1 → 3. Card buffers 3/3; duration 30.178 s; capture/output drops 0/0; host peak 3/10; discarded trace events 0.

Timing statistics exclude the first 1 second(s) and shutdown. Counts and drop totals above include startup.

| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |
|---|---:|---:|---:|---:|---:|---:|
| capture_dma | 1459 | 1.462 | 1.511 | 1.696 | 0 | 0 |
| capture_processing | 1459 | 0.019 | 0.024 | 0.054 | 0 | 0 |
| input_device_wait | 1458 | 18.474 | 18.512 | 18.864 | 0 | 0 |
| capture_host_wait | 1458 | 0.001 | 0.002 | 0.003 | 0 | 0 |
| output_device_wait | 1459 | 18.307 | 18.361 | 18.394 | 0 | 0 |
| output_host_wait | 1459 | 0.001 | 0.001 | 0.002 | 0 | 0 |
| output_dma | 1459 | 1.651 | 1.701 | 1.974 | 0 | 0 |

Host occupancy range in recorded operation-boundary snapshots:
- host_capture: 0–1
- host_ready: 1–2
- host_output: 0–1
- host_occupied: 1–2

DMA measurements are synchronous API elapsed times, including driver/scheduler overhead. Wait measurements include normal pacing and event/lock delays. These are not hardware-only DMA timings or OS scheduling traces. Occupancy snapshots near stage transitions are approximate and event-weighted.

### sdi-17918951200394633-trace

SDI 1 → 4; frame stores 1 → 3. Card buffers 4/4; duration 60.103 s; capture/output drops 0/0; host peak 2/10; discarded trace events 0.

Timing statistics exclude the first 1 second(s) and shutdown. Counts and drop totals above include startup.

| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |
|---|---:|---:|---:|---:|---:|---:|
| input_device_wait | 2955 | 18.471 | 18.509 | 18.811 | 0 | 0 |
| capture_host_wait | 2955 | 0.001 | 0.002 | 0.003 | 0 | 0 |
| capture_dma | 2955 | 1.466 | 1.514 | 1.898 | 0 | 0 |
| capture_processing | 2955 | 0.019 | 0.024 | 0.053 | 0 | 0 |
| output_device_wait | 2955 | 18.304 | 18.361 | 18.864 | 0 | 0 |
| output_host_wait | 2955 | 0.001 | 0.002 | 0.005 | 0 | 0 |
| output_dma | 2955 | 1.652 | 1.705 | 2.263 | 0 | 0 |

Host occupancy range in recorded operation-boundary snapshots:
- host_capture: 0–1
- host_ready: 0–1
- host_output: 0–1
- host_occupied: 0–1

DMA measurements are synchronous API elapsed times, including driver/scheduler overhead. Wait measurements include normal pacing and event/lock delays. These are not hardware-only DMA timings or OS scheduling traces. Occupancy snapshots near stage transitions are approximate and event-weighted.

### sdi-17918951802718438-trace

SDI 1 → 4; frame stores 1 → 3. Card buffers 7/7; duration 30.245 s; capture/output drops 0/0; host peak 3/10; discarded trace events 0.

Timing statistics exclude the first 1 second(s) and shutdown. Counts and drop totals above include startup.

| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |
|---|---:|---:|---:|---:|---:|---:|
| input_device_wait | 1463 | 18.464 | 18.506 | 19.328 | 0 | 0 |
| capture_host_wait | 1463 | 0.001 | 0.002 | 0.003 | 0 | 0 |
| capture_dma | 1463 | 1.467 | 1.518 | 1.974 | 0 | 0 |
| capture_processing | 1463 | 0.019 | 0.024 | 0.054 | 0 | 0 |
| output_host_wait | 1463 | 18.130 | 18.212 | 19.146 | 0 | 0 |
| output_dma | 1462 | 1.848 | 1.915 | 3.017 | 0 | 0 |

Host occupancy range in recorded operation-boundary snapshots:
- host_capture: 0–1
- host_ready: 0–1
- host_output: 0–1
- host_occupied: 0–1

DMA measurements are synchronous API elapsed times, including driver/scheduler overhead. Wait measurements include normal pacing and event/lock delays. These are not hardware-only DMA timings or OS scheduling traces. Occupancy snapshots near stage transitions are approximate and event-weighted.
