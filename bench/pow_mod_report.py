#!/usr/bin/env python3
"""pow_mod_report.py — Generate Markdown tables and SVG chart from bench_pow_mod JSON output.

Usage:
    ./build-rel/bench_pow_mod --json > results.json
    python3 bench/pow_mod_report.py results.json
    python3 bench/pow_mod_report.py results.json --chart pow_mod_chart.svg
    python3 bench/pow_mod_report.py results.json --output report.md
"""

import json
import sys
import argparse
import math


def format_ns(ns):
    if ns < 1_000:
        return f"{ns:.1f} ns"
    elif ns < 1_000_000:
        return f"{ns/1e3:.2f} µs"
    elif ns < 1_000_000_000:
        return f"{ns/1e6:.2f} ms"
    else:
        return f"{ns/1e9:.3f} s"


def format_ops(ops):
    if ops >= 1e6:
        return f"{ops/1e6:.2f}M"
    elif ops >= 1e3:
        return f"{ops/1e3:.2f}K"
    else:
        return f"{ops:.1f}"


def generate_markdown(data):
    lines = []
    results = data["results"]
    backends = data["backends"]

    lines.append("## pow_mod Benchmark — Big Integer Modular Arithmetic Comparison\n")
    lines.append(f"**Operation:** `pow_mod(base, exp, mod)` where base, exp, mod are all N-bit\n")
    lines.append(f"**Samples:** {data['samples_per_width']} per width per backend | **Metric:** median latency\n")

    # --- Median ---
    lines.append("### Median Latency\n")
    header = "| Width "
    sep = "|------:"
    for b in backends:
        header += f"| {b:^14s} "
        sep += "|:--------------:"
    header += "|"
    sep += "|"
    lines.append(header)
    lines.append(sep)

    for r in results:
        row = f"| {r['bits']:>5} "
        for b in backends:
            val = r.get(b, {}).get("median_ns", 0)
            row += f"| {format_ns(val):>14s} "
        row += "|"
        lines.append(row)

    # --- P95 ---
    lines.append("\n### P95 Latency\n")
    lines.append(header)
    lines.append(sep)
    for r in results:
        row = f"| {r['bits']:>5} "
        for b in backends:
            val = r.get(b, {}).get("p95_ns", 0)
            row += f"| {format_ns(val):>14s} "
        row += "|"
        lines.append(row)

    # --- Ops/sec ---
    lines.append("\n### Throughput (ops/sec)\n")
    lines.append(header)
    lines.append(sep)
    for r in results:
        row = f"| {r['bits']:>5} "
        for b in backends:
            val = r.get(b, {}).get("ops_per_sec", 0)
            row += f"| {format_ops(val):>14s} "
        row += "|"
        lines.append(row)

    # --- Relative (if more than one backend) ---
    if len(backends) > 1:
        lines.append("\n### Relative Performance (vs Hydra median, negative = Hydra faster)\n")
        header2 = "| Width "
        sep2 = "|------:"
        for b in backends[1:]:
            header2 += f"| {b + ' delta':^16s} "
            sep2 += "|:----------------:"
        header2 += "|"
        sep2 += "|"
        lines.append(header2)
        lines.append(sep2)

        for r in results:
            hydra_ns = r.get("hydra", {}).get("median_ns", 1)
            row = f"| {r['bits']:>5} "
            for b in backends[1:]:
                other_ns = r.get(b, {}).get("median_ns", 1)
                delta = ((hydra_ns / other_ns) - 1.0) * 100.0
                row += f"| {delta:>+14.1f}% "
            row += "|"
            lines.append(row)

    lines.append("")
    lines.append("> **Note:** This benchmark compares big integer modular arithmetic performance,")
    lines.append("> not production cryptographic suitability.")

    return "\n".join(lines)


def generate_svg_chart(data, width=800, height=480):
    """Generate a log-scale bar chart as SVG comparing backends across widths."""
    results = data["results"]
    backends = data["backends"]

    colors = {
        "hydra":        "#2563EB",  # blue
        "boost_cpp_int":"#DC2626",  # red
        "gmp":          "#16A34A",  # green
        "openssl":      "#D97706",  # amber
    }

    margin_left = 100
    margin_right = 30
    margin_top = 60
    margin_bottom = 70
    plot_w = width - margin_left - margin_right
    plot_h = height - margin_top - margin_bottom

    # Collect all median values for scale
    all_vals = []
    for r in results:
        for b in backends:
            v = r.get(b, {}).get("median_ns", 0)
            if v > 0:
                all_vals.append(v)

    if not all_vals:
        return "<svg></svg>"

    log_min = math.floor(math.log10(min(all_vals)))
    log_max = math.ceil(math.log10(max(all_vals)))
    if log_min == log_max:
        log_max += 1

    n_widths = len(results)
    n_backends = len(backends)
    group_width = plot_w / n_widths
    bar_width = max(8, (group_width * 0.7) / n_backends)
    group_gap = group_width * 0.3

    def y_pos(val):
        if val <= 0:
            return margin_top + plot_h
        lv = math.log10(val)
        frac = (lv - log_min) / (log_max - log_min)
        return margin_top + plot_h * (1.0 - frac)

    svg_parts = []
    svg_parts.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
                     f'font-family="system-ui, -apple-system, sans-serif">')

    # Background
    svg_parts.append(f'<rect width="{width}" height="{height}" fill="#FAFAFA" rx="8"/>')

    # Title
    svg_parts.append(f'<text x="{width/2}" y="30" text-anchor="middle" '
                     f'font-size="16" font-weight="600" fill="#111">pow_mod Median Latency (log scale)</text>')

    # Y-axis grid lines and labels
    for exp in range(log_min, log_max + 1):
        y = y_pos(10**exp)
        svg_parts.append(f'<line x1="{margin_left}" y1="{y}" x2="{width-margin_right}" y2="{y}" '
                         f'stroke="#E5E7EB" stroke-width="1"/>')
        label = format_ns(10**exp)
        svg_parts.append(f'<text x="{margin_left-10}" y="{y+4}" text-anchor="end" '
                         f'font-size="11" fill="#6B7280">{label}</text>')

    # Bars
    for wi, r in enumerate(results):
        group_x = margin_left + wi * group_width + group_gap / 2
        for bi, b in enumerate(backends):
            val = r.get(b, {}).get("median_ns", 0)
            if val <= 0:
                continue
            x = group_x + bi * bar_width
            y = y_pos(val)
            bar_h = (margin_top + plot_h) - y
            color = colors.get(b, "#888")
            svg_parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_width:.1f}" '
                             f'height="{bar_h:.1f}" fill="{color}" rx="2" opacity="0.85"/>')

        # Width label
        cx = group_x + (n_backends * bar_width) / 2
        svg_parts.append(f'<text x="{cx:.1f}" y="{margin_top+plot_h+20}" text-anchor="middle" '
                         f'font-size="12" fill="#374151">{r["bits"]}-bit</text>')

    # Legend
    legend_y = height - 20
    legend_x = margin_left
    for bi, b in enumerate(backends):
        x = legend_x + bi * 150
        color = colors.get(b, "#888")
        svg_parts.append(f'<rect x="{x}" y="{legend_y-8}" width="12" height="12" '
                         f'fill="{color}" rx="2"/>')
        svg_parts.append(f'<text x="{x+16}" y="{legend_y+2}" font-size="11" '
                         f'fill="#374151">{b}</text>')

    svg_parts.append('</svg>')
    return '\n'.join(svg_parts)


def main():
    parser = argparse.ArgumentParser(description="Generate pow_mod benchmark report")
    parser.add_argument("input", help="JSON file from bench_pow_mod")
    parser.add_argument("--output", "-o", help="Markdown output file (default: stdout)")
    parser.add_argument("--chart", help="SVG chart output file")
    args = parser.parse_args()

    with open(args.input) as f:
        data = json.load(f)

    md = generate_markdown(data)

    if args.output:
        with open(args.output, "w") as f:
            f.write(md + "\n")
        print(f"Report written to {args.output}", file=sys.stderr)
    else:
        print(md)

    if args.chart:
        svg = generate_svg_chart(data)
        with open(args.chart, "w") as f:
            f.write(svg + "\n")
        print(f"Chart written to {args.chart}", file=sys.stderr)


if __name__ == "__main__":
    main()
