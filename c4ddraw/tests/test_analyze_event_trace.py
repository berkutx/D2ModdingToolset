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


if __name__ == "__main__":
    unittest.main()
