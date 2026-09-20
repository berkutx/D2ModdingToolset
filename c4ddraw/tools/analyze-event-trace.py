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
NETWORK_PAIRS = {202: (203, "network_send"), 220: (221, "turn_info"),
                 223: (224, "disconnect")}
ALL_PAIRS = dict(PAIRS)
ALL_PAIRS.update(NETWORK_PAIRS)
RETURNS = {end: (start, name) for start, (end, name) in ALL_PAIRS.items()}
FRAME_KINDS = {0: "none", 1: "BeginTurn", 2: "EndTurn", 3: "TurnInfo"}
FRAME_STATUS = {0: "empty", 1: "incomplete_header", 2: "invalid_header", 3: "unselected",
                4: "selected_incomplete", 5: "selected_too_large", 6: "selected_complete"}
SITES = {1: "client_send", 2: "server_send", 4: "client_receive", 8: "server_receive",
         16: "client_count", 32: "server_count"}


def signed32(value):
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def site_names(mask):
    return [name for bit, name in SITES.items() if mask & bit]


def decode_network(rows, warn):
    """Decode one process only. An ordinal relates local records, never peers."""
    events, coverage, calls = [], [], []
    latest_counters = {}
    frames = collections.defaultdict(list)
    calls_by_key = collections.defaultdict(list)
    for row in rows:
        event = row["event"]
        if not 200 <= event <= 239:
            continue
        item = {key: row[key] for key in ("seq", "qpc", "tick", "tid", "event", "object")}
        a, b, c, d = (row[key] for key in ("a", "b", "c", "d"))
        role = {0: "client", 1: "server"}.get(b, "unknown_{}".format(b))
        if event == 200:
            item.update(name="network_boundary_coverage", installed_mask=a,
                        installed_sites=site_names(a))
            coverage.append(item)
        elif event == 201:
            item.update(name="network_boundary_unavailable", reason_code=a,
                        reason={1: "unsupported_exe", 2: "changed_callsite", 3: "detour_transaction",
                                4: "unsupported_architecture"}.get(a, "unknown"),
                        site_mask=b, sites=site_names(b), error=c)
            coverage.append(item)
        elif event in (202, 204):
            item.update(name="send_enter" if event == 202 else "receive_success",
                        ordinal=a, role=role, target=d)
            if event == 202:
                item["recipient"] = c
            else:
                item.update(sender_raw=c, sender=None, sender_available=None)
            calls.append(item)
            calls_by_key[(row["tid"], row["object"], a)].append(item)
        elif event == 203:
            item.update(name="send_result", ordinal=a, role=role,
                        accepted=bool(c), result_raw=c, last_error=d)
        elif event == 205:
            # Two records describe one frame on a 32-bit process. Do not infer a
            # digest from a lost half or from an incomplete/oversized frame.
            frames[(row["tid"], row["object"], a)].append(row)
            continue
        elif event == 208:
            item.update(name="network_exception", ordinal=a, role=role,
                        operation={0: "send", 1: "receive", 2: "count"}.get(c, "unknown"), target=d)
        elif event in (210, 211, 212):
            counter_role = {0: "client", 1: "server"}.get(row["object"], "unknown_{}".format(row["object"]))
            if event == 210:
                group = "calls"
                item.update(name="network_call_counters", sends=a, send_failures=b,
                            receives=c, receive_success=d)
            elif event == 211:
                group = "receives"
                item.update(name="network_receive_counters", receive_empty=a, receive_failures=b,
                            count_calls=c, last_observed_count=signed32(d))
            else:
                group = "observer"
                item.update(name="network_observer_counters", active_calls=a, exceptions=b,
                            selected=c, malformed=d)
            item["role"] = counter_role
            latest_counters.setdefault(counter_role, {})[group] = item
        elif event in (220, 221):
            item.update(name="turn_info_enter" if event == 220 else "turn_info_return",
                        native_message=a, ordinal=d)
            if event == 220:
                item.update(native_owner=b, native_message_vtable=c)
            else:
                item.update(result=signed32(b), normal_return=bool(c))
        elif event == 222:
            item.update(name="turn_info_coverage", available=bool(a), exact_exe=bool(b))
            coverage.append(item)
        elif event in (223, 224):
            item.update(name="disconnect_enter" if event == 223 else "disconnect_return",
                        net_player_id=a, ordinal=d)
            if event == 224:
                item["normal_return"] = bool(c)
        elif event == 225:
            status = signed32(a)
            item.update(name="disconnect_coverage", status=status, available=status == 1,
                        vtable_slot=b, reason={0: "not_attempted", 1: "installed", -1: "unsupported_exe",
                        -2: "changed_entry_or_vtable", -3: "detour_failed"}.get(status, "unknown"))
            coverage.append(item)
        elif event == 230:
            item.update(name="ui_heartbeat", ui_dispatches=a, registered_message_dispatches=b,
                        turn_info_available=bool(c), exact_exe=bool(d))
            latest_counters["ui"] = item
        else:
            item.update(name="unknown_network_event", a=a, b=b, c=c, d=d)
        events.append(item)

    unmatched_frames = []
    for key, fragments in frames.items():
        candidates = calls_by_key.get(key, [])
        classification = [fragment for fragment in fragments if fragment["b"] != 0x80000000]
        fingerprints = [fragment for fragment in fragments if fragment["b"] == 0x80000000]
        if len(candidates) != 1 or len(classification) != 1 or len(fingerprints) > 1:
            warn("ambiguous_network_frame_records", tid=key[0], object=key[1], ordinal=key[2],
                 boundaries=len(candidates), classifications=len(classification), fingerprints=len(fingerprints))
            unmatched_frames.extend(fragments)
            continue
        frame = classification[0]
        kind, status = frame["b"] & 0xFF, (frame["b"] >> 8) & 0xFF
        decoded = {"kind": FRAME_KINDS.get(kind, "unknown"), "kind_code": kind,
                   "status": FRAME_STATUS.get(status, "unknown"), "status_code": status,
                   "length": frame["c"], "fingerprint": None,
                   "classification_seq": frame["seq"], "fingerprint_seq": None,
                   "sender_status": frame["d"]}
        if kind not in FRAME_KINDS or status not in FRAME_STATUS or frame["b"] >> 16:
            warn("unknown_network_frame_classification", seq=frame["seq"], classification=frame["b"])
        if fingerprints:
            fingerprint = fingerprints[0]
            decoded["fingerprint_seq"] = fingerprint["seq"]
            if fingerprint["c"] > 0xFFFFFFFF or fingerprint["d"] > 0xFFFFFFFF:
                warn("invalid_network_fingerprint_halves", seq=fingerprint["seq"])
            elif status == 6 and kind in (1, 2, 3):
                decoded["fingerprint"] = "{:016x}".format((fingerprint["d"] << 32) | fingerprint["c"])
        elif status == 6:
            warn("missing_network_fingerprint", seq=frame["seq"], ordinal=key[2])
        candidates[0]["frame"] = decoded
        if candidates[0]["event"] == 204:
            candidates[0]["sender_available"] = frame["d"] == 1
            if frame["d"] == 1:
                candidates[0]["sender"] = candidates[0]["sender_raw"]
    for key, candidates in calls_by_key.items():
        if key not in frames:
            for item in candidates:
                warn("missing_network_frame_records", seq=item["seq"], ordinal=item["ordinal"])

    # A send's accepted result is useful beside its fingerprint. Only join one
    # exact same-process invocation; a repeated/wrapped ordinal remains ambiguous.
    for item in events:
        if item["event"] != 203:
            continue
        candidates = calls_by_key.get((item["tid"], item["object"], item["ordinal"]), [])
        if len(candidates) == 1 and candidates[0]["event"] == 202 and candidates[0]["role"] == item["role"]:
            target = candidates[0]
            if "send_result" in target:
                target["send_result"] = None
                warn("ambiguous_network_send_result", seq=item["seq"], ordinal=item["ordinal"])
            else:
                target["send_result"] = {key: item[key] for key in ("seq", "accepted", "result_raw", "last_error")}

    return {"events": events, "coverage": coverage, "selected_calls": calls,
            "latest_counters": latest_counters, "unmatched_frame_records": unmatched_frames,
            "interpretation": "Local call ordinals and fingerprints correlate observations, not delivery. "
            "send accepted is not a transport acknowledgement. No cross-PC clock matching is performed. "
            "Counters wrap at 2^32 and are sampled independently; last_observed_count is not a live queue probe. "
            "Missing receive leaves sending MSS, transport/lobby and receiving MSS unresolved."}


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
        if event in ALL_PAIRS:
            _, name = ALL_PAIRS[event]
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
            elif name == "network_send":
                matching = matching and all(entered[key] == row[key] for key in ("a", "b"))
            elif name in ("turn_info", "disconnect"):
                matching = matching and all(entered[key] == row[key] for key in ("a", "d"))
            if not matching:
                unpaired.append({"kind": "return_identity_mismatch", "family": name, **row})
                continue # do not guess a deeper match across a missing event
            stack.pop()
            delta = row["qpc"] - entered["qpc"]
            span = {"family": name, "tid": row["tid"], "object": row["object"],
                          "enter_seq": entered["seq"], "return_seq": row["seq"],
                          "enter_qpc": entered["qpc"], "return_qpc": row["qpc"],
                          "qpc_ticks": delta,
                          "milliseconds": delta * 1000.0 / frequency if frequency else None}
            if name == "network_send":
                span.update(ordinal=row["a"], role=entered["b"], recipient=entered["c"],
                            accepted=bool(row["c"]), last_error=row["d"])
            elif name in ("turn_info", "disconnect"):
                span.update(ordinal=row["d"], normal_return=bool(row["c"]))
                if name == "turn_info":
                    span.update(native_message=entered["a"], native_owner=entered["b"],
                                result=signed32(row["b"]))
                else:
                    span["net_player_id"] = entered["a"]
            spans.append(span)
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
    network = decode_network(sorted_rows, warn)
    partial = bool(warnings)
    return {"source": source, "metadata": metadata, "records": len(rows),
            "qpc_frequency": frequency, "sorted_by": ["qpc", "tid", "seq"],
            "csv_order_was_changed": reordered, "event_counts": dict(sorted(counts.items())),
            "loss_counters_max_observed": counters, "sequence_gaps": sequence_gaps,
            "footer_present": bool(stops), "partial_or_ambiguous": partial,
            "warnings": warnings, "duration_summaries": summaries, "observed_spans": spans,
            "unpaired_boundaries": unpaired,
            "network": network,
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
        network = result["network"]
        if network["events"]:
            for coverage in network["coverage"]:
                print("COVERAGE " + json.dumps(coverage, sort_keys=True))
            for call in network["selected_calls"][-20:]:
                print("NETWORK " + json.dumps(call, sort_keys=True))
            if len(network["selected_calls"]) > 20:
                print("Showing last 20 selected calls; --json includes all observed calls.")
            print("LATEST COUNTERS " + json.dumps(network["latest_counters"], sort_keys=True))
            print(network["interpretation"])
        for warning in result["warnings"]:
            print("WARNING " + json.dumps(warning, sort_keys=True))
        print(result["interpretation"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
