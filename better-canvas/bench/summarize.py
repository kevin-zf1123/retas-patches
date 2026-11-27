"""Print one CSV row per gesture. Times are milliseconds, sizes MiB."""
import argparse
import csv
import gzip
import json
import math
from pathlib import Path
import sys


def percentile(values, fraction):
    return sorted(values)[math.ceil(fraction * len(values)) - 1] if values else None


def summarize(data, kind):
    ticks = data["frequency"] / 1000
    times = dict(native=0, read=0, scan=0, mix=0)
    inputs = paints = blocks = repeated = allocated = failures = 0
    intervals, latency, settle, private, free = [], [], [], [], []
    cpu, hard, gpu, anchors = [], [], [], []
    for run in data["runs"]:
        if run["kind"] != kind:
            continue
        events = run["events"]
        starts = [e[0] for e in events if e[1] == 12]
        assert len(starts) == len(run["posted"])
        inputs += len(starts)
        # Exclude the idle refinement paint from interactive latency.
        ends = []
        refining = False
        last_refine = None
        seen = set()
        for event, following in zip(events, events[1:]):
            t, code, a, b, _ = event
            if code == 12:
                refining = False
            if code == 14:
                refining = True
                last_refine = t
            if code == 9 and not refining:
                ends.append(t)
            if code == 9:
                seen.clear()
            if refining:
                continue
            if code == 3:
                allocated += a
                failures += not b
            if code == 4:
                blocks += 1
                repeated += (a,b) in seen
                seen.add((a,b))
            stage = None
            if code == 1:
                stage = "native"
            elif code in (2,3,4) and following[1] in (3,4,5):
                stage = "read"
            elif code == 5:
                stage = "scan"
            elif code == 6:
                stage = "mix"
            if stage:
                times[stage] += (following[0] - t) / ticks
        if events[-1][1] == 9 and not refining:
            ends.append(events[-1][0])
        # Assign each completed paint to the latest dispatched input. Earlier
        # coalesced inputs have no independently observable frame.
        last = -1
        visible = []
        for end in ends:
            idx = sum(t <= end for t in starts) - 1
            if idx > last:
                visible.append(end)
                latency.append((end - run["posted"][idx]) / ticks)
                last = idx
        paints += len(visible)
        intervals += [(b-a)/ticks for a,b in zip(visible,visible[1:])]
        if last_refine and last_refine >= starts[-1]:
            completed = [e[0] for e in events if e[1] == 18 and e[0] >= last_refine]
            if not completed:
                completed = [e[0] for e in events if e[1] == 9 and e[0] >= last_refine]
            if completed:
                settle.append((completed[0] - run["posted"][-1])/ticks)
        private += [m["private"]/2**20 for m in (run["before"], run["after"])]
        free += [m["largest_free"]/2**20 for m in (run["before"], run["after"])]
        begin, end = run["before"], run["after"]
        if "ui_cpu_100ns" in begin:
            cpu.append((end["ui_cpu_100ns"]-begin["ui_cpu_100ns"])/10000)
        if isinstance(begin.get("hard_faults"),int) and isinstance(end.get("hard_faults"),int):
            hard.append(end["hard_faults"]-begin["hard_faults"])
        if begin.get("gpu") and end.get("gpu"):
            common = begin["gpu"].keys() & end["gpu"].keys()
            gpu.append(sum(end["gpu"][k]-begin["gpu"][k] for k in common)/10000)
        a = [e[2:4] for e in events if e[1] == 15]
        b = [e[2:4] for e in events if e[1] == 16]
        anchors.extend(max(abs(x-y) for x,y in zip(i,j))/256 for i,j in zip(a,b))
    return dict(kind=kind, inputs=inputs, paints=paints,
                interval_p50=percentile(intervals,.5), interval_p95=percentile(intervals,.95),
                latency_p50=percentile(latency,.5), latency_p95=percentile(latency,.95),
                settle_p50=percentile(settle,.5), settle_p95=percentile(settle,.95),
                **{k:v/inputs for k,v in times.items()}, blocks=blocks, repeated=repeated,
                alloc_mib_per_input=allocated/2**20/inputs, failures=failures,
                max_sampled_private_mib=max(private), min_sampled_free_mib=min(free),
                ui_ms_per_input=sum(cpu)/inputs if cpu else None, hard_faults=sum(hard) if hard else None,
                gpu_engine_ms=sum(gpu) if gpu else None, anchor_max_px=max(anchors) if anchors else None)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("traces", nargs="+", type=Path)
    cli.add_argument("--combine", action="store_true", help="Pool repeated internal traces of the same condition")
    args = cli.parse_args()
    rows = []
    combined = None
    for trace in args.traces:
        with gzip.open(trace, "rt", encoding="utf-8") as fp:
            data = json.load(fp)
        if args.combine:
            if combined is None:
                combined = data
            else:
                for key in ("document_sha256","mode","fast","frequency","viewport","reference","duplicate_layers"):
                    if data.get(key) != combined.get(key):
                        raise ValueError("Mismatched traces: " + key)
                combined["runs"].extend(data["runs"])
        elif "frequency" in data:
            rows.extend(dict(file=trace.name, **summarize(data,k)) for k in ("rotate","zoom"))
        else:
            for kind,runs in data.items():
                if not isinstance(runs,list) or not runs or not isinstance(runs[0],dict):
                    continue
                intervals, latency, settle, reads = [], [], [], []
                for r in runs:
                    valid = [(s,e) for s,e,changed in r["samples"] if changed]
                    latency.extend((e-s)/1e6 for s,e in valid)
                    intervals.extend((b[1]-a[1])/1e6 for a,b in zip(valid,valid[1:]))
                    reads.extend(t/1e6 for t in r["reads"])
                    if valid:
                        final = r["tail"][-1][1]
                        last = max((i for i,v in enumerate(r["tail"]) if v[1]!=final),default=-1)
                        settle.append(0 if last<0 and final==r["held"] else (r["tail"][last+1][0]-r["release"])/1e6)
                rows.append(dict(file=trace.name,kind=kind, inputs=sum(len(r["samples"]) for r in runs),
                                 observed_updates=len(latency), interval_p50=percentile(intervals,.5),
                                 interval_p95=percentile(intervals,.95), latency_p50=percentile(latency,.5),
                                 latency_p95=percentile(latency,.95), settle_p50=percentile(settle,.5),
                                 settle_p95=percentile(settle,.95), read_p50=percentile(reads,.5)))
    if combined:
        rows.extend(dict(file="+".join(p.name for p in args.traces),**summarize(combined,k)) for k in ("rotate","zoom"))
    writer = csv.DictWriter(sys.stdout,fieldnames=list(dict.fromkeys(key for row in rows for key in row)))
    writer.writeheader()
    writer.writerows({k:round(v,3) if isinstance(v,float) else v for k,v in row.items()} for row in rows)


if __name__ == "__main__":
    main()
