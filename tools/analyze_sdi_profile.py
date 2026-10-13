"""Summarize bounded per-call SDI traces using only the Python standard library."""
import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path


def percentile(values, fraction):
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def analyze(trace_path, warmup):
    sample_path = trace_path.with_name(trace_path.name.replace("-trace.csv", ".csv"))
    summary_path = trace_path.with_name(trace_path.name.replace("-trace.csv", "-profile.csv"))
    with sample_path.open(newline="") as stream:
        samples = list(csv.DictReader(stream))
    last = samples[-1]
    route = ""
    if "input_frame_store" in last:
        route = (f"SDI {last['input_sdi']} → {last['output_sdi']}; "
                 f"frame stores {last['input_frame_store']} → {last['output_frame_store']}. ")
    with trace_path.open(newline="") as stream:
        events = [row for row in csv.DictReader(stream)
                  if row["shutdown"] == "0" and float(row["end_seconds"]) >= warmup]
    with summary_path.open(newline="") as stream:
        summary = list(csv.DictReader(stream))
    discarded = next((row["count"] for row in summary if row["event"] == "trace_discarded"), "unknown")
    grouped = defaultdict(list)
    failures = defaultdict(int)
    for row in events:
        grouped[row["event"]].append(float(row["duration_ms"]))
        failures[row["event"]] += row["success"] == "0"
    lines = [f"### {trace_path.stem}", "",
             route + f"Card buffers {last['input_capacity']}/{last['output_capacity']}; "
             f"duration {last['seconds']} s; capture/output drops "
             f"{last['input_drops']}/{last['output_drops']}; "
             f"host peak {last['host_peak']}/10; discarded trace events {discarded}.", "",
             f"Timing statistics exclude the first {warmup:g} second(s) and shutdown. "
             "Counts and drop totals above include startup.", "",
             "| Operation | Calls | Avg ms | P95 ms | Max ms | >20 ms | Failed |",
             "|---|---:|---:|---:|---:|---:|---:|"]
    for name, values in grouped.items():
        if name in {"host_publish", "host_release"}:
            continue
        values.sort()
        lines.append(f"| {name} | {len(values)} | {sum(values)/len(values):.3f} | "
                     f"{percentile(values, .95):.3f} | {values[-1]:.3f} | "
                     f"{sum(value > 20 for value in values)} | {failures[name]} |")
    lines += ["", "Host occupancy range in recorded operation-boundary snapshots:"]
    for field in ["host_capture", "host_ready", "host_output", "host_occupied"]:
        values = [int(row[field]) for row in events]
        if values:
            lines.append(f"- {field}: {min(values)}–{max(values)}")
    lines += ["", "DMA measurements are synchronous API elapsed times, including driver/scheduler "
              "overhead. Wait measurements include normal pacing and event/lock delays. "
              "These are not hardware-only DMA timings or OS scheduling traces. "
              "Occupancy snapshots near stage transitions are approximate and event-weighted.", ""]
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--warmup", type=float, default=1.0)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    report = "# SDI profiling comparison\n\n" + "\n".join(
        analyze(path, arguments.warmup) for path in arguments.traces)
    if arguments.output:
        arguments.output.write_text(report, encoding="utf-8")
    else:
        print(report)
