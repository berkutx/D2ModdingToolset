#!/usr/bin/env python3
"""Read C4trace v1 CSV without modifying it or inferring a causal diagnosis.

Python 3.7+ standard library only. Times are observed same-thread call spans,
not network delivery, item latency, pixel visibility, or proof of a flag reset.
"""
import argparse
import collections
import csv
import json
import pathlib
import statistics
import sys


COLUMNS = "seq qpc tick tid event object a b c d".split()
PAIRS = {3: (4, "post"), 22: (23, "native"), 110: (111, "send"),
         118: (119, "apply"), 120: (121, "notify")}
RETURNS = {end: (start, name) for start, (end, name) in PAIRS.items()}


def number(value):
    value = value.strip()
    result = int(value, 16 if value.lower().startswith("0x") else 10)
    if result < 0 or result > 0xFFFFFFFFFFFFFFFF:
        raise ValueError("field outside unsigned 64-bit range")
    return result


def analyze(lines, source="<stream>"):
    metadata, rows, warnings = [], [], []
    header_seen = False
    stop_seen = False

    def warn(code, **details):
        warnings.append(dict(code=code, **details))

    for line_number, line in enumerate(lines, 1):
        if line_number == 1:
            line = line.lstrip("\ufeff")
        if not line.strip():
            continue
        try:
            values = next(csv.reader([line], strict=True))
        except (csv.Error, StopIteration) as error:
            warn("malformed_csv", line=line_number, detail=str(error))
            continue
        if values and values[0].startswith("#"):
            metadata.append({"line": line_number, "key": values[0][1:],
                             "values": values[1:]})
            stop_seen = stop_seen or values[0] == "#stop"
            continue
        if values == COLUMNS:
            if header_seen:
                warn("duplicate_header", line=line_number)
            header_seen = True
            continue
        if not header_seen:
            warn("row_before_header", line=line_number)
        if len(values) != len(COLUMNS):
            warn("malformed_row", line=line_number, fields=len(values))
            continue
        try:
            row = dict(zip(COLUMNS, (number(value) for value in values)))
            if row["seq"] == 0 or row["tid"] == 0:
                raise ValueError("seq and tid must be positive")
        except ValueError as error:
            warn("malformed_number", line=line_number, detail=str(error))
            continue
        row["line"] = line_number
        if stop_seen:
            warn("row_after_stop", line=line_number)
        rows.append(row)

    by_key = collections.defaultdict(list)
    for entry in metadata:
        by_key[entry["key"]].append(entry)
    if not header_seen:
        warn("missing_header")
    schemas = by_key.get("schema", [])
    if len(schemas) != 1 or schemas[0]["values"] != ["C4trace", "1"]:
        warn("unsupported_or_missing_schema")
    frequency = None
    frequencies = by_key.get("qpc_frequency", [])
    try:
        if len(frequencies) != 1 or len(frequencies[0]["values"]) != 1:
            raise ValueError()
        frequency = number(frequencies[0]["values"][0])
        if frequency <= 0:
            raise ValueError()
    except ValueError:
        frequency = None
        warn("invalid_or_missing_qpc_frequency")

    counters = {"lock_drops": 0, "buffer_drops": 0, "unwritten": 0}
    stops = []
    for entry in metadata:
        if entry["key"] not in ("status", "stop"):
            continue
        fields = {}
        for value in entry["values"]:
            if "=" not in value:
                warn("malformed_status", line=entry["line"])
                continue
            key, value = value.split("=", 1)
            fields[key] = value
            if key in counters:
                try:
                    counters[key] = max(counters[key], number(value))
                except ValueError:
                    warn("malformed_counter", line=entry["line"], counter=key)
        if entry["key"] == "stop":
            stops.append(fields)
    if not stops:
        warn("open_tail", detail="No stop footer; buffered tail and final loss counters are unknown.")
    if len(stops) > 1:
        warn("multiple_stop_footers")
    stop = stops[-1] if stops else None
    if stop:
        if stop.get("reason") != "test_stop":
            warn("recorder_stopped", reason=stop.get("reason", "missing"))
        try:
            if number(stop.get("win32_error", "0")):
                warn("writer_error", win32_error=stop["win32_error"])
            if "written" in stop and number(stop["written"]) != len(rows):
                warn("written_count_mismatch", footer=number(stop["written"]), rows=len(rows))
        except ValueError:
            warn("malformed_stop_counter")
    if any(counters.values()):
        warn("records_lost_or_unwritten", **counters)

    sequence_counts = collections.Counter(row["seq"] for row in rows)
    duplicate_sequences = sum(count - 1 for count in sequence_counts.values())
    if duplicate_sequences:
        warn("duplicate_sequences", duplicates=duplicate_sequences)
    sequence_gaps = 0
    if sequence_counts:
        # Sequence starts at one; include an absent prefix but do not invent a tail.
        sequence_gaps = max(sequence_counts) - len(sequence_counts)
        if sequence_gaps:
            warn("sequence_gaps", missing_through_max_observed=sequence_gaps)

    sorted_rows = sorted(rows, key=lambda row: (row["qpc"], row["tid"], row["seq"]))
    reordered = any(left["line"] != right["line"] for left, right in zip(rows, sorted_rows))
    counts = collections.Counter(row["event"] for row in rows)
    stacks = collections.defaultdict(list)
    spans, unpaired = [], []
    # Post/native can nest. LIFO matching is only within the same TID and family.
    # Check preserved argument identity, never infer missing scopes or item joins.
    for row in sorted_rows:
        event = row["event"]
        if event in PAIRS:
            _, name = PAIRS[event]
            stacks[(row["tid"], name)].append(row)
        elif event in RETURNS:
            _, name = RETURNS[event]
            stack = stacks[(row["tid"], name)]
            if not stack:
                unpaired.append({"kind": "return_without_enter", "family": name, **row})
                continue
            entered = stack[-1]
            matching = entered["object"] == row["object"]
            if name in ("post", "native"):
                matching = matching and entered["a"] == row["a"] # message ID
            elif name in ("send", "notify"):
                matching = matching and all(entered[key] == row[key] for key in ("a", "b", "c", "d"))
            if not matching:
                unpaired.append({"kind": "return_identity_mismatch", "family": name, **row})
                continue # do not guess a deeper match across a missing event
            stack.pop()
            delta = row["qpc"] - entered["qpc"]
            spans.append({"family": name, "tid": row["tid"], "object": row["object"],
                          "enter_seq": entered["seq"], "return_seq": row["seq"],
                          "enter_qpc": entered["qpc"], "return_qpc": row["qpc"],
                          "qpc_ticks": delta,
                          "milliseconds": delta * 1000.0 / frequency if frequency else None})
    for (_, name), stack in sorted(stacks.items()):
        unpaired.extend({"kind": "enter_without_return", "family": name, **row} for row in stack)
    if unpaired:
        warn("unpaired_boundaries", count=len(unpaired))

    groups = collections.defaultdict(list)
    for span in spans:
        groups[(span["tid"], span["family"])].append(span["qpc_ticks"])
    summaries = []
    for (tid, name), ticks in sorted(groups.items()):
        summary = {"tid": tid, "family": name, "count": len(ticks),
                   "min_qpc_ticks": min(ticks), "median_qpc_ticks": statistics.median(ticks),
                   "max_qpc_ticks": max(ticks)}
        if frequency:
            summary.update({key.replace("qpc_ticks", "ms"): summary[key] * 1000.0 / frequency
                            for key in ("min_qpc_ticks", "median_qpc_ticks", "max_qpc_ticks")})
        summaries.append(summary)
    partial = bool(warnings)
    return {"source": source, "metadata": metadata, "records": len(rows),
            "qpc_frequency": frequency, "sorted_by": ["qpc", "tid", "seq"],
            "csv_order_was_changed": reordered, "event_counts": dict(sorted(counts.items())),
            "loss_counters_max_observed": counters, "sequence_gaps": sequence_gaps,
            "footer_present": bool(stops), "partial_or_ambiguous": partial,
            "warnings": warnings, "duration_summaries": summaries, "observed_spans": spans,
            "unpaired_boundaries": unpaired,
            "interpretation": "Observed same-thread call spans only; not a causal diagnosis, "
                              "network delivery measurement, item latency, or proof of callback reset. "
                              "No warning means internally consistent observed data, not full session coverage."}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=pathlib.Path, help="C4trace CSV; read-only")
    parser.add_argument("--json", action="store_true", help="include raw metadata and every observed span")
    options = parser.parse_args(argv)
    try:
        with options.trace.open("r", encoding="utf-8-sig", newline="") as stream:
            result = analyze(stream, str(options.trace))
    except (OSError, UnicodeError) as error:
        parser.exit(2, "Cannot read trace: {}\n".format(error))
    if options.json:
        print(json.dumps(result, ensure_ascii=True, indent=2))
    else:
        print("{} records; {}; sorted by QPC/TID/seq".format(
            result["records"], "PARTIAL/AMBIGUOUS" if result["partial_or_ambiguous"] else "consistent observed data"))
        print("Events: " + ", ".join("{}={}".format(key, value) for key, value in result["event_counts"].items()))
        for group in result["duration_summaries"]:
            if result["qpc_frequency"]:
                print("TID {tid} {family}: n={count} min/median/max="
                      "{min_ms:.3f}/{median_ms:.3f}/{max_ms:.3f} ms".format(**group))
            else:
                print("TID {tid} {family}: n={count} min/median/max="
                      "{min_qpc_ticks}/{median_qpc_ticks}/{max_qpc_ticks} QPC ticks".format(**group))
        for warning in result["warnings"]:
            print("WARNING " + json.dumps(warning, sort_keys=True))
        print(result["interpretation"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
