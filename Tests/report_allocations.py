#!/usr/bin/env python3
"""Attribute macOS full malloc history from a real GlooPrint benchmark.

Capture with MallocStackLogging=full, MallocStackLoggingNoCompact=1 and
leaks --fullStackHistory --outputGraph=... --atExit -- UnrealEditor ... .
The capture must use -ansimalloc. This reports recorded heap request sizes,
not resident memory, allocator metadata, VM mappings, or untraced allocations.
Tracing changes timing; use separate uninstrumented runs for performance.
"""

import argparse
import csv
import gzip
import json
import re
import subprocess
from collections import Counter, defaultdict
from pathlib import Path


def phase(stack, native=False):
    if "GlooPrint" not in stack:
        return None
    # Check routing first: ComputeLayoutRoutes contains "ComputeLayout".
    if any(s in stack for s in ("GlooPrint::ComputeLayoutRoutes(",
                               "GlooPrint::ComputeRoutes(", "GlooPrint::FRoutingJob::")):
        return "routing"
    if any(s in stack for s in ("GlooPrint::ComputeLayout(", "GlooPrint::FLayoutJob::")):
        return "layout"
    if native:
        # Exclusive allocation-site attribution: a child phase wins over its
        # containing capture/format/rebuild callback. Native lifetimes can span
        # several callbacks, so same-phase freeing is not a temporary-call proof.
        for name, functions in (
            ("application", ("GlooPrint::ApplyLayout(",)),
            ("measurement", ("GlooPrint::MeasureNode(", "GlooPrint::FMeasurementJob::", "GlooPrint::MeasureGraph(")),
            ("snapshot", ("GlooPrint::FGraphCaptureJob::", "GlooPrint::BuildSnapshot(")),
            ("measurement_reuse", ("GlooPrint::FMeasurementCache::PrepareLayoutReuse(", "GlooPrint::FMeasurementCache::RestoreLayoutReuse(")),
            ("signature", ("GlooPrint::CaptureMeasurementState(", "CaptureLayoutInvariantState(")),
            ("validation", ("GlooPrint::ValidateMeasurementGraph(",)),
            ("planning", ("GlooPrint::FFormatJob::",)),
            ("format_overhead", ("GlooPrint::FEditor::",)),
            ("route_cache_overhead", ("GlooPrint::FRouteCache::",)),
            ("measurement_cache_overhead", ("GlooPrint::FMeasurementCache::",)),
        ):
            if any(function in stack for function in functions):
                return name
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("memgraph", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--scope", default="FPipelineBenchmark::Update()")
    parser.add_argument("--native", action="store_true",
                        help="Include native phases; use --scope '' for automatic rebuilds and all F callbacks")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    allocation = re.compile(r"^ALLOC (0x[0-9a-f]+)(?:-0x[0-9a-f]+)? \[size=(\d+)\]:\s*(.*)")
    free = re.compile(r"^(?:FREE|DEALLOC)\s+(0x[0-9a-f]+):\s*(.*)")
    live, stacks, stats, types = {}, {}, defaultdict(Counter), Counter()
    records = []
    errors = []
    event_path = args.output / "events.csv.gz"
    with (args.output / "malloc-history.log").open("w") as log, gzip.open(event_path, "wt") as events:
        process = subprocess.Popen(["malloc_history", str(args.memgraph), "-allEvents", "-noContent"],
                                   stdout=subprocess.PIPE, stderr=log, text=True)
        writer = csv.writer(events)
        writer.writerow(["event", "allocation", "kind"])
        for event, line in enumerate(process.stdout):
            types[line.split(" ", 1)[0]] += 1
            match = allocation.match(line)
            if match:
                address, size, stack = match.groups()
                if address in live:
                    errors.append(f"Live address allocated again at event {event}: {address}")
                category = phase(stack, args.native)
                if category and args.scope in stack:
                    size = int(size)
                    stack_id = stacks.setdefault(stack, len(stacks))
                    index = len(records)
                    records.append([category, size, stack_id, "unfreed"])
                    live[address] = index
                    stat = stats[category]
                    stat["allocations"] += 1
                    stat["allocated_bytes"] += size
                    stat["live_bytes"] += size
                    stat["peak_live_bytes"] = max(stat["peak_live_bytes"], stat["live_bytes"])
                    writer.writerow([event, index, "allocate"])
            else:
                match = free.match(line)
                if match and match[1] in live:
                    index = live.pop(match[1])
                    category, size, _, _ = records[index]
                    # A buffer freed inside its solver was temporary (including
                    # replaced output buffers). A buffer surviving that return
                    # is labeled returned. This describes lifetime, not ownership:
                    # profiler/global buffers can also survive. Use the free stack
                    # rather than subtracting final output from an earlier peak.
                    same_phase = phase(match[2], args.native) == category
                    lifetime = (("same_phase" if same_phase else "other_phase") if args.native
                                else ("temporary" if same_phase else "returned"))
                    records[index][3] = lifetime
                    stats[category]["live_bytes"] -= size
                    writer.writerow([event, index, "free"])
        return_code = process.wait()
    if return_code:
        errors.append(f"malloc_history exited {return_code}")
    if not records:
        errors.append("No symbolicated phase allocations matched the requested scope")
    # Replay after lifetimes are known to measure simultaneous temporary usage.
    for category, size, _, lifetime in records:
        stats[f"{category}.{lifetime}"]["allocations"] += 1
        stats[f"{category}.{lifetime}"]["allocated_bytes"] += size
    with gzip.open(event_path, "rt") as events:
        for row in csv.DictReader(events):
            category, size, _, lifetime = records[int(row["allocation"])]
            stat = stats[f"{category}.{lifetime}"]
            stat["live_bytes"] += size if row["kind"] == "allocate" else -size
            stat["peak_live_bytes"] = max(stat["peak_live_bytes"], stat["live_bytes"])
    with gzip.open(args.output / "allocations.csv.gz", "wt") as output:
        writer = csv.writer(output)
        writer.writerow(["phase", "bytes", "stack", "lifetime"])
        writer.writerows(records)
    (args.output / "stacks.json").write_text(json.dumps(list(stacks), indent=2) + "\n")
    summary = {"memgraph": str(args.memgraph.resolve()), "scope": args.scope, "native": args.native,
               "stats": dict(stats), "record_types": dict(types), "errors": errors}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({"stats": dict(stats), "errors": errors}, indent=2))
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
