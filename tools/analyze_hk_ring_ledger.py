#!/usr/bin/env python3
"""Classify bounded HyperBridge Unity Gfx-ring ledger records from launch.stderr.

The logger writes records as key=value fields and may use a literal ``\\n``
between records, so parsing is performed over the whole file rather than by
physical lines. Expected values compare against previous_consumed, whose range
starts before the dispatcher's common opcode read. They therefore include the
common 4-byte opcode advance plus the case payload. The fixed-size watchlist is
seeded from HK-GFX-COMMAND-LENGTH-TABLE.md; extend it with --expected CMD:BYTES
only for another statically proven total footprint.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


PREFIX_RE = re.compile(r"macrunner-hb-gfx-ring-ledger:\s+stage=(armed|handler-entry|handler-return)\b")
FIELD_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)=([^\s\\]+)")
# Full predecessor footprints, not payload-only sizes. Command 10077 is absent:
# its length depends on a count field captured from the command payload.
DEFAULT_EXPECTED_BYTES = {
    10007: 8,
    10022: 76,
    10027: 8,
    10028: 20,
    10035: 48,
    10054: 4,
    10123: 4,
    10237: 16,
}


@dataclass(frozen=True)
class Record:
    stage: str
    fields: dict[str, str]
    offset: int

    def int(self, key: str) -> Optional[int]:
        value = self.fields.get(key)
        if value is None:
            return None
        try:
            return int(value, 16) if value.lower().startswith("0x") else int(value, 10)
        except ValueError:
            return None


@dataclass
class Sample:
    seq: Optional[int]
    entry: Optional[Record] = None
    returned: Optional[Record] = None


def parse_expected(values: list[str]) -> dict[int, int]:
    expected = dict(DEFAULT_EXPECTED_BYTES)
    for item in values:
        command_text, separator, bytes_text = item.partition(":")
        if not separator:
            raise ValueError(f"--expected needs CMD:BYTES, got {item!r}")
        try:
            command = int(command_text, 0)
            byte_count = int(bytes_text, 0)
        except ValueError as error:
            raise ValueError(f"invalid --expected {item!r}: {error}") from error
        if command < 0 or byte_count < 0:
            raise ValueError(f"--expected values must be non-negative: {item!r}")
        expected[command] = byte_count
    return expected


def parse_records(text: str) -> list[Record]:
    matches = list(PREFIX_RE.finditer(text))
    records: list[Record] = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        fields = dict(FIELD_RE.findall(text[match.start() : end]))
        fields["stage"] = match.group(1)
        records.append(Record(match.group(1), fields, match.start()))
    return records


def pair_samples(records: list[Record]) -> list[Sample]:
    samples: list[Sample] = []
    open_entries: dict[int, Sample] = {}
    for record in records:
        if record.stage == "armed":
            continue
        seq = record.int("seq")
        if record.stage == "handler-entry":
            sample = Sample(seq=seq, entry=record)
            samples.append(sample)
            if seq is not None:
                open_entries[seq] = sample
        elif record.stage == "handler-return":
            sample = open_entries.pop(seq, None) if seq is not None else None
            if sample is None:
                sample = Sample(seq=seq, returned=record)
                samples.append(sample)
            else:
                sample.returned = record
    return samples


def fmt_hex(value: Optional[int]) -> str:
    return "?" if value is None else f"0x{value:x}"


def fmt_decimal(value: Optional[int]) -> str:
    return "?" if value is None else str(value)


def publication_stable(entry: Optional[Record]) -> Optional[bool]:
    if entry is None:
        return None
    value = entry.int("publication_stable")
    return None if value is None else value != 0


def predecessor_measurement(sample: Sample, expected: dict[int, int]) -> tuple[Optional[int], Optional[int], Optional[int], str]:
    """Return command, observed bytes, expected bytes, and provenance.

    A matching completed predecessor is the ledger's direct consumed-range
    witness for last_command.  The fallback command itself has no expected
    size unless a caller supplied one and a handler-return record is present.
    """
    entry = sample.entry
    if entry is not None:
        previous_command = entry.int("previous_cmd")
        previous_consumed = entry.int("previous_consumed")
        previous_complete = entry.int("previous_complete")
        previous_matches_last = entry.int("previous_matches_last")
        if (
            previous_command in expected
            and previous_consumed is not None
            and previous_complete == 1
            and previous_matches_last == 1
        ):
            return previous_command, previous_consumed, expected[previous_command], "predecessor"

        command = entry.int("cmd_edx")
        if command in expected and sample.returned is not None:
            consumed = sample.returned.int("consumed_bytes")
            if consumed is not None:
                return command, consumed, expected[command], "handler-return"
    return None, None, None, "unmapped"


def unreadable_next_words(entry: Optional[Record]) -> bool:
    """Conservative stale-metadata witness; raw words have no known grammar."""
    if entry is None:
        return False
    next_ok = entry.int("next_ok")
    next_words_valid = entry.int("next_words_valid")
    return next_ok == 0 or (next_words_valid is not None and (next_words_valid & 0x7) != 0x7)


def sample_row(sample: Sample, expected: dict[int, int]) -> tuple[list[str], dict[str, object]]:
    entry = sample.entry
    returned = sample.returned
    command, consumed, expected_bytes, source = predecessor_measurement(sample, expected)
    cmd_edx = entry.int("cmd_edx") if entry else None
    last_command = entry.int("last_command") if entry else None
    cursor_before = entry.int("cursor_before") if entry else None
    cursor_after = (
        returned.int("cursor_after_handler")
        if returned is not None
        else (entry.int("cursor_after_dispatch") if entry else None)
    )
    stable = publication_stable(entry)
    event = f"seq={sample.seq if sample.seq is not None else '?'}"
    event += ":entry+return" if entry and returned else (":entry" if entry else ":return")
    row = [
        event,
        fmt_decimal(cmd_edx),
        fmt_decimal(last_command),
        fmt_hex(cursor_before),
        fmt_hex(cursor_after),
        fmt_decimal(consumed),
        fmt_decimal(expected_bytes),
        "yes" if stable is True else ("no" if stable is False else "?"),
    ]
    return row, {
        "command": command,
        "consumed": consumed,
        "expected": expected_bytes,
        "stable": stable,
        "next_unreadable": unreadable_next_words(entry),
        "source": source,
    }


def print_table(headers: list[str], rows: list[list[str]]) -> None:
    widths = [len(header) for header in headers]
    for row in rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))
    print("  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
    print("  ".join("-" * width for width in widths))
    for row in rows:
        print("  ".join(value.ljust(widths[index]) for index, value in enumerate(row)))


def verdict(records: list[Record], samples: list[Sample], expected: dict[int, int]) -> tuple[str, str]:
    armed = sum(record.stage == "armed" for record in records)
    if not samples:
        suffix = "stage=armed present; hook was enabled but no handler-entry/handler-return record was emitted." if armed else "stage=armed absent; hook did not arm or this is not a ledger log."
        return "INCONCLUSIVE", suffix

    evidence = [sample_row(sample, expected)[1] for sample in samples]
    decode = [
        item
        for item in evidence
        if item["expected"] is not None
        and item["consumed"] != item["expected"]
        and item["stable"] is True
    ]
    if decode:
        return "DECODE_CURSOR_BUG", "stable publication and a proven command length disagree with the consumed range."

    stale = [
        item
        for item in evidence
        if item["expected"] is not None
        and item["consumed"] == item["expected"]
        and (item["stable"] is False or item["next_unreadable"])
    ]
    if stale:
        return "STALE_METADATA", "consumed range matches its proven size, but publication is unstable or next-word reads are invalid."

    unstable_mismatch = [
        item
        for item in evidence
        if item["expected"] is not None
        and item["consumed"] != item["expected"]
        and item["stable"] is not True
    ]
    if unstable_mismatch:
        return "INCONCLUSIVE", "a consumed-range mismatch exists, but publication is not stably observed; it cannot be attributed to decode."

    return "INCONCLUSIVE", "records exist but no completed, matched predecessor has a configured proven total footprint."


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stderr", type=Path, help="run's launch.stderr")
    parser.add_argument(
        "--expected",
        action="append",
        default=[],
        metavar="CMD:BYTES",
        help="add/override a statically proven total command footprint (repeatable)",
    )
    args = parser.parse_args(argv)

    try:
        expected = parse_expected(args.expected)
    except ValueError as error:
        parser.error(str(error))
    try:
        text = args.stderr.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        parser.error(f"cannot read {args.stderr}: {error}")

    records = parse_records(text)
    samples = pair_samples(records)
    armed = sum(record.stage == "armed" for record in records)
    entries = sum(record.stage == "handler-entry" for record in records)
    returns = sum(record.stage == "handler-return" for record in records)

    print(f"HK ring-ledger: {args.stderr}")
    print(f"records: armed={armed} handler-entry={entries} handler-return={returns} samples={len(samples)}")
    print("expected bytes: " + ", ".join(f"{command}={length}" for command, length in sorted(expected.items())))
    if samples:
        print()
        print_table(
            ["event", "cmd_edx", "last_command", "cursor_before", "cursor_after", "consumed", "expected", "publication_stable"],
            [sample_row(sample, expected)[0] for sample in samples],
        )
    result, reason = verdict(records, samples, expected)
    print()
    print(f"VERDICT: {result}")
    print(f"REASON: {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
