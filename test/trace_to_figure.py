#!/usr/bin/env python3

import argparse
import html
from pathlib import Path

import yaml


def parse_args():
    parser = argparse.ArgumentParser(
        description="Render a small YAML trace as an SVG transaction timeline."
    )
    parser.add_argument("trace", type=Path, help="YAML trace to render")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="SVG output path. Defaults to <trace-stem>.svg",
    )
    parser.add_argument(
        "--key-label",
        action="append",
        default=[],
        metavar="HEX=LABEL",
        help="Map a key hex string to a display label, e.g. --key-label 01=X",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=None,
        help="Canvas width in pixels. Defaults to a width based on session count.",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=700,
        help="Canvas height in pixels.",
    )
    parser.add_argument(
        "--session-gap",
        type=int,
        default=360,
        help="Horizontal distance between session lifelines.",
    )
    parser.add_argument(
        "--show-sessions",
        action="store_true",
        help="Show session labels above lifelines.",
    )
    return parser.parse_args()


def normalize_hex(value):
    if value is None:
        return ""
    if isinstance(value, int):
        text = format(value, "x")
    else:
        text = str(value).strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    text = "".join(text.split())
    return text.lstrip("0") or "0"


def parse_key_labels(items):
    labels = {}
    for item in items:
        if "=" not in item:
            raise SystemExit(f"--key-label must use HEX=LABEL, got: {item}")
        key, label = item.split("=", 1)
        labels[normalize_hex(key)] = label
    return labels


def value_label(event):
    if event.get("delete"):
        return "DEL"
    if event.get("miss"):
        return "miss"
    value = event.get("value_hex")
    if value is None:
        return "?"

    raw = normalize_hex(value)
    if len(raw) <= 8:
        return str(int(raw, 16))
    return raw[:12] + "..."


def load_trace(path):
    with path.open("r", encoding="utf-8") as trace_file:
        documents = [doc for doc in yaml.safe_load_all(trace_file) if doc]

    txns = []
    for idx, doc in enumerate(documents):
        events = sorted(doc.get("events", []), key=lambda event: event.get("seq", 0))
        txns.append(
            {
                "index": idx,
                "label": f"T{idx + 1}",
                "session_id": doc["session_id"],
                "start_ts": int(doc["start_ts"]),
                "commit_ts": int(doc["commit_ts"]),
                "events": events,
            }
        )
    return txns


def svg_text(x, y, text, anchor="start", size=20, weight="normal"):
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" '
        f'font-family="Arial, Helvetica, sans-serif" font-size="{size}" '
        f'font-weight="{weight}">{html.escape(text)}</text>'
    )


def svg_line(x1, y1, x2, y2, marker_end=False, width=2):
    marker = ' marker-end="url(#arrow)"' if marker_end else ""
    return (
        f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
        f'stroke="#000" stroke-width="{width}"{marker}/>'
    )


def event_label(event, key_labels, read_number):
    key = key_labels.get(normalize_hex(event.get("key_hex")), str(event.get("key_hex")))
    value = value_label(event)
    if event.get("op") == "R":
        return f"r{read_number} = {key} // {value}"
    return f"{key} = {value}"


def render_svg(txns, key_labels, width, height, show_sessions, session_gap):
    sessions = sorted({txn["session_id"] for txn in txns})
    x_margin = 190
    if width is None:
        width = max(760, 2 * x_margin + (len(sessions) - 1) * session_gap)

    if len(sessions) == 1:
        x_positions = {sessions[0]: width / 2}
    else:
        gap = (width - 2 * x_margin) / (len(sessions) - 1)
        x_positions = {
            session_id: x_margin + idx * gap
            for idx, session_id in enumerate(sessions)
        }

    min_ts = min(txn["start_ts"] for txn in txns)
    max_ts = max(txn["commit_ts"] for txn in txns)
    top = 80
    bottom = height - 80
    span = max(max_ts - min_ts, 1)

    def y_for_ts(timestamp):
        return top + (timestamp - min_ts) * (bottom - top) / span

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        "<defs>",
        '<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" '
        'markerWidth="7" markerHeight="7" orient="auto-start-reverse">',
        '<path d="M 0 0 L 10 5 L 0 10 z" fill="#000"/>',
        "</marker>",
        "</defs>",
        '<rect x="0" y="0" width="100%" height="100%" fill="#fff"/>',
    ]

    for session_id in sessions:
        x = x_positions[session_id]
        parts.append(svg_line(x, top - 35, x, bottom + 35, width=2))
        if show_sessions:
            parts.append(svg_text(x, top - 48, f"S{session_id}", anchor="middle", size=16))

    txns_by_session = {session_id: [] for session_id in sessions}
    for txn in txns:
        txns_by_session[txn["session_id"]].append(txn)

    for session_index, session_id in enumerate(sessions):
        x = x_positions[session_id]
        event_x = x + 82
        event_anchor = "start"
        label_x = x - 52 if session_index == 0 else x + 52
        label_anchor = "end" if session_index == 0 else "start"

        for txn in txns_by_session[session_id]:
            start_y = y_for_ts(txn["start_ts"])
            commit_y = y_for_ts(txn["commit_ts"])
            parts.append(svg_text(label_x, start_y + 6, txn["label"], label_anchor, 20))
            parts.append(svg_line(x - 20, start_y, x + 20, start_y, marker_end=True))
            parts.append(svg_line(x + 20, commit_y, x - 20, commit_y, marker_end=True))

            events = txn["events"]
            read_number = 0
            for event_index, event in enumerate(events):
                event_y = start_y + (event_index + 1) * (commit_y - start_y) / (
                    len(events) + 1
                )
                if event.get("op") == "R":
                    read_number += 1
                text = event_label(event, key_labels, read_number)
                parts.append(svg_text(event_x, event_y + 6, text, event_anchor, 20))

    parts.append("</svg>")
    return "\n".join(parts)


def main():
    args = parse_args()
    txns = load_trace(args.trace)
    key_labels = parse_key_labels(args.key_label)
    output = args.output or args.trace.with_suffix(".svg")
    svg = render_svg(
        txns,
        key_labels,
        args.width,
        args.height,
        args.show_sessions,
        args.session_gap,
    )
    output.write_text(svg, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
