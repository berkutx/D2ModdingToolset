"""Synthetic CSV tests only; does not read a running game or write trace files."""
import importlib.util
import pathlib
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "tools" / "analyze-event-trace.py"
SPEC = importlib.util.spec_from_file_location("analyze_event_trace", str(SCRIPT))
TRACE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRACE)

HEADER = "#schema,C4trace,1\n#pid,123\n#qpc_frequency,1000\n" + ",".join(TRACE.COLUMNS) + "\n"


def row(seq, qpc, tid, event, obj=100, a=200, b=0, c=0, d=0):
    return "{},{},0,{},{},0x{:X},0x{:X},0x{:X},0x{:X},0x{:X}\n".format(seq, qpc, tid, event, obj, a, b, c, d)


def footer(written, reason="test_stop", **extra):
    values = dict(reason=reason, win32_error=0, lock_drops=0, buffer_drops=0,
                  accepted=written, written=written, unwritten=0)
    values.update(extra)
    return "#stop," + ",".join("{}={}".format(key, value) for key, value in values.items()) + "\n"


def analyze(body, end="", header=HEADER):
    return TRACE.analyze((header + body + end).splitlines(True))


def codes(result):
    return {warning["code"] for warning in result["warnings"]}


class AnalyzerTests(unittest.TestCase):
    def test_reorders_physical_csv_and_matches_thread_not_global_stack(self):
        body = row(4, 40, 2, 4) + row(2, 20, 2, 3) + row(3, 30, 1, 4) + row(1, 10, 1, 3)
        result = analyze(body, footer(4))
        self.assertTrue(result["csv_order_was_changed"])
        self.assertFalse(result["partial_or_ambiguous"])
        self.assertEqual([span["milliseconds"] for span in result["observed_spans"]], [20, 20])
        self.assertEqual([span["tid"] for span in result["observed_spans"]], [1, 2])

    def test_lifo_reentrant_same_tid_native(self):
        body = row(1, 1, 4, 22, obj=10) + row(2, 2, 4, 22, obj=11)
        body += row(3, 5, 4, 23, obj=11) + row(4, 9, 4, 23, obj=10)
        result = analyze(body, footer(4))
        self.assertEqual([span["qpc_ticks"] for span in result["observed_spans"]], [3, 8])
        self.assertFalse(result["warnings"])

    def test_all_families_and_metadata_preserved(self):
        body = ""
        for index, (enter, (leave, _)) in enumerate(TRACE.PAIRS.items()):
            body += row(index * 2 + 1, index * 10, 5, enter)
            body += row(index * 2 + 2, index * 10 + 2, 5, leave)
        result = analyze(body, footer(10))
        self.assertEqual({span["family"] for span in result["observed_spans"]},
                         {"post", "native", "send", "apply", "notify"})
        self.assertEqual(result["metadata"][1]["values"], ["123"])
        self.assertEqual(len(result["event_counts"]), 10)

    def test_post_returns_do_not_compare_changed_result_error_arguments(self):
        result = analyze(row(1, 1, 5, 3, b=444, c=555) + row(2, 8, 5, 4, b=1, c=0), footer(2))
        self.assertEqual(len(result["observed_spans"]), 1)

    def test_no_guess_when_return_identity_differs(self):
        result = analyze(row(1, 1, 2, 110, c=11) + row(2, 3, 2, 111, c=12), footer(2))
        self.assertFalse(result["observed_spans"])
        self.assertIn("unpaired_boundaries", codes(result))
        self.assertEqual({entry["kind"] for entry in result["unpaired_boundaries"]},
                         {"return_identity_mismatch", "enter_without_return"})

    def test_open_tail_and_unpaired_return(self):
        result = analyze(row(1, 5, 3, 119) + row(2, 7, 3, 120))
        self.assertIn("open_tail", codes(result))
        self.assertEqual(len(result["unpaired_boundaries"]), 2)
        self.assertTrue(result["partial_or_ambiguous"])

    def test_drop_counters_cap_and_unwritten_are_explicit(self):
        body = row(1, 1, 1, 3) + "#status,lock_drops=2,buffer_drops=3,written=1\n"
        body += row(7, 10, 1, 4)
        result = analyze(body, footer(2, "cap", lock_drops=2, buffer_drops=3, unwritten=4))
        self.assertEqual(result["loss_counters_max_observed"], {"lock_drops": 2, "buffer_drops": 3, "unwritten": 4})
        self.assertEqual(result["sequence_gaps"], 5)
        self.assertTrue({"records_lost_or_unwritten", "sequence_gaps", "recorder_stopped"}.issubset(codes(result)))

    def test_malformed_input_and_missing_frequency_keep_raw_counts(self):
        body = row(1, 1, 1, 118) + "bad,row\n" + row(2, 3, 1, 119)
        body += '"unterminated\n' + "0,0,0,0,0,0,0,0,0,0\n"
        result = analyze(body, footer(2), HEADER.replace("#qpc_frequency,1000\n", ""))
        self.assertEqual(result["records"], 2)
        self.assertIsNone(result["observed_spans"][0]["milliseconds"])
        self.assertTrue({"malformed_row", "malformed_csv", "malformed_number", "invalid_or_missing_qpc_frequency"}.issubset(codes(result)))

    def test_duplicate_sequence_and_footer_count(self):
        result = analyze(row(1, 1, 1, 3) + row(1, 2, 1, 4), footer(9))
        self.assertIn("duplicate_sequences", codes(result))
        self.assertIn("written_count_mismatch", codes(result))

    def test_sort_tie_uses_tid_then_seq_not_csv_order(self):
        result = analyze(row(2, 10, 1, 111) + row(1, 10, 1, 110), footer(2))
        self.assertEqual(result["observed_spans"][0]["qpc_ticks"], 0)
        self.assertFalse(result["warnings"])

    def test_no_item_or_causal_inference_keys(self):
        result = analyze(row(1, 1, 1, 110) + row(2, 2, 1, 111), footer(2))
        self.assertNotIn("root_cause", result)
        self.assertNotIn("item_latency", result)
        self.assertIn("not a causal diagnosis", result["interpretation"])

    def test_numeric_overflow_is_malformed_not_a_float_crash(self):
        result = analyze(row(1, 10 ** 400, 1, 3), footer(0))
        self.assertEqual(result["records"], 0)
        self.assertIn("malformed_number", codes(result))

    def test_selected_send_frame_digest_and_result_share_only_local_ordinal(self):
        body = row(4, 40, 1, 203, a=7, b=1, c=0, d=123)
        body += row(1, 10, 1, 202, a=7, b=1, c=45, d=0x70001234)
        body += row(3, 30, 1, 205, a=7, b=0x80000000, c=0x89ABCDEF, d=0x1234567)
        body += row(2, 20, 1, 205, a=7, b=2 | (6 << 8), c=57)
        result = analyze(body, footer(4))
        self.assertFalse(result["warnings"])
        call = result["network"]["selected_calls"][0]
        self.assertEqual((call["role"], call["recipient"], call["frame"]["kind"]),
                         ("server", 45, "EndTurn"))
        self.assertEqual(call["frame"]["fingerprint"], "0123456789abcdef")
        self.assertFalse(call["send_result"]["accepted"])
        self.assertEqual(call["send_result"]["last_error"], 123)
        span = result["observed_spans"][0]
        self.assertEqual((span["family"], span["qpc_ticks"], span["ordinal"]), ("network_send", 30, 7))
        self.assertFalse(span["accepted"])
        self.assertNotIn("delivered", call)

    def test_receive_uses_successful_boundary_and_explicit_sender_validity(self):
        body = row(1, 10, 1, 204, a=9, b=0, c=1, d=0x70009999)
        body += row(2, 11, 1, 205, a=9, b=3 | (6 << 8), c=64, d=1)
        body += row(3, 12, 1, 205, a=9, b=0x80000000, c=123, d=456)
        body += row(4, 20, 1, 204, a=10, b=0, c=0, d=0x70009999)
        body += row(5, 21, 1, 205, a=10, b=3 | (6 << 8), c=64, d=2)
        body += row(6, 22, 1, 205, a=10, b=0x80000000, c=123, d=456)
        result = analyze(body, footer(6))
        first, second = result["network"]["selected_calls"]
        self.assertEqual(first["name"], "receive_success")
        self.assertEqual(first["sender"], 1)
        self.assertTrue(first["sender_available"])
        self.assertIsNone(second["sender"])
        self.assertFalse(second["sender_available"])
        self.assertEqual(first["frame"]["fingerprint"], second["frame"]["fingerprint"])
        self.assertEqual(len(result["network"]["selected_calls"]), 2)
        self.assertFalse(result["observed_spans"])
        self.assertFalse(result["warnings"])

    def test_incomplete_frame_does_not_publish_placeholder_digest(self):
        body = row(1, 1, 1, 204, a=1, b=0, c=1)
        body += row(2, 2, 1, 205, a=1, b=3 | (4 << 8), c=64, d=1)
        body += row(3, 3, 1, 205, a=1, b=0x80000000, c=0, d=0)
        result = analyze(body, footer(3))
        frame = result["network"]["selected_calls"][0]["frame"]
        self.assertEqual(frame["status"], "selected_incomplete")
        self.assertIsNone(frame["fingerprint"])

    def test_missing_and_ambiguous_frame_parts_never_guess_fingerprint(self):
        body = row(1, 1, 1, 204, a=1, b=0, c=1)
        body += row(2, 2, 1, 205, a=1, b=3 | (6 << 8), c=64, d=1)
        result = analyze(body, footer(2))
        self.assertIn("missing_network_fingerprint", codes(result))
        self.assertIsNone(result["network"]["selected_calls"][0]["frame"]["fingerprint"])
        # A same-numbered ordinal on another TID is not its missing half.
        body += row(3, 3, 2, 205, a=1, b=0x80000000, c=8, d=9)
        result = analyze(body, footer(3))
        self.assertIn("ambiguous_network_frame_records", codes(result))
        self.assertEqual(len(result["network"]["unmatched_frame_records"]), 1)
        self.assertIsNone(result["network"]["selected_calls"][0]["frame"]["fingerprint"])

    def test_duplicate_boundary_ordinal_stays_ambiguous(self):
        body = row(1, 1, 1, 204, a=1, b=0, c=1)
        body += row(2, 2, 1, 204, a=1, b=0, c=1)
        body += row(3, 3, 1, 205, a=1, b=1 | (6 << 8), c=56, d=1)
        body += row(4, 4, 1, 205, a=1, b=0x80000000, c=0, d=0)
        result = analyze(body, footer(4))
        self.assertIn("ambiguous_network_frame_records", codes(result))
        self.assertTrue(all("frame" not in call for call in result["network"]["selected_calls"]))

    def test_turn_info_and_disconnect_spans_keep_native_result_and_abnormal_return(self):
        body = row(1, 10, 4, 220, a=0x1000, b=0xA3DE0001, c=0x6D4B14, d=11)
        body += row(2, 20, 4, 223, obj=120, a=1, d=12)
        body += row(3, 30, 4, 224, obj=120, a=1, b=0, c=0, d=12)
        body += row(4, 40, 4, 221, a=0x1000, b=0xFFFFFFFE, c=1, d=11)
        result = analyze(body, footer(4))
        disconnected, turn = result["observed_spans"]
        self.assertEqual(disconnected["family"], "disconnect")
        self.assertFalse(disconnected["normal_return"])
        self.assertEqual(disconnected["net_player_id"], 1)
        self.assertEqual(turn["family"], "turn_info")
        self.assertEqual(turn["result"], -2)
        self.assertEqual(turn["native_owner"], 0xA3DE0001)
        self.assertTrue(turn["normal_return"])
        self.assertNotIn("new_owner", turn)
        self.assertFalse(result["warnings"])

    def test_turn_info_requires_exact_ordinal_and_message_for_return(self):
        body = row(1, 10, 1, 220, a=0x1000, d=5)
        body += row(2, 20, 1, 221, a=0x1000, d=6)
        result = analyze(body, footer(2))
        self.assertFalse(result["observed_spans"])
        self.assertIn("unpaired_boundaries", codes(result))

    def test_network_coverage_and_latest_counters_remain_observations(self):
        body = row(1, 1, 1, 200, obj=0, a=1 | 4 | 16)
        body += row(2, 2, 1, 201, obj=0x43373B, a=2, b=2, c=0)
        body += row(3, 3, 1, 222, obj=0x48A680, a=1, b=1)
        body += row(4, 4, 1, 225, obj=0x40C2BE, a=0xFFFFFFFE, b=0x6CEB54)
        body += row(5, 5, 1, 230, a=100, b=10, c=1, d=1)
        body += row(6, 6, 1, 210, obj=0, a=11, b=2, c=99, d=40)
        body += row(7, 7, 1, 211, obj=0, a=50, b=9, c=12, d=0xFFFFFFFF)
        body += row(8, 8, 1, 212, obj=0, a=0, b=1, c=3, d=0)
        body += row(9, 9, 1, 210, obj=1, a=8, b=0, c=18, d=10)
        body += row(10, 10, 1, 230, a=200, b=20, c=1, d=1)
        result = analyze(body, footer(10))
        network = result["network"]
        self.assertEqual(network["coverage"][0]["installed_sites"],
                         ["client_send", "client_receive", "client_count"])
        self.assertEqual(network["coverage"][1]["reason"], "changed_callsite")
        self.assertFalse(network["coverage"][3]["available"])
        self.assertEqual(network["coverage"][3]["status"], -2)
        counters = network["latest_counters"]
        self.assertEqual(counters["ui"]["ui_dispatches"], 200)
        self.assertEqual(counters["client"]["calls"]["receive_success"], 40)
        self.assertEqual(counters["client"]["receives"]["last_observed_count"], -1)
        self.assertEqual(counters["server"]["calls"]["sends"], 8)
        self.assertNotIn("current_queue_depth", counters["client"]["receives"])
        self.assertNotIn("root_cause", network)
        self.assertIn("No cross-PC clock matching", network["interpretation"])
        self.assertFalse(result["warnings"])

    def test_unknown_network_event_preserved_and_invalid_digest_half_reported(self):
        body = row(1, 1, 1, 239, a=42)
        body += row(2, 2, 1, 204, a=1, b=0, c=1)
        body += row(3, 3, 1, 205, a=1, b=3 | (6 << 8), c=64, d=1)
        body += row(4, 4, 1, 205, a=1, b=0x80000000, c=0x100000000, d=0)
        result = analyze(body, footer(4))
        self.assertEqual(result["network"]["events"][0]["name"], "unknown_network_event")
        self.assertEqual(result["network"]["events"][0]["a"], 42)
        self.assertIn("invalid_network_fingerprint_halves", codes(result))
        self.assertIsNone(result["network"]["selected_calls"][0]["frame"]["fingerprint"])


if __name__ == "__main__":
    unittest.main()
