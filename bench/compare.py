#!/usr/bin/env python3
"""
bench/compare.py — Hydra benchmark comparison and delta report

Reads a Google Benchmark JSON export, groups results into families, and
produces a human-readable comparison table showing:

  • Small ops  : % overhead vs. native uint64_t  (baseline/* family)
  • Medium/Large: % overhead vs. Boost.Multiprecision (boost/* family)
  • Allocation, copy/move, chain: standalone cost tables

Usage
─────
  # 1. Run benchmarks with JSON output:
  ./build-rel/hydra_bench --benchmark_format=json \\
                          --benchmark_out=results.json

  # 2. Compare:
  python3 bench/compare.py results.json

  # 3. Markdown table (pipe into a file for README snippets):
  python3 bench/compare.py results.json --markdown > bench_report.md

  # 4. Save to file:
  python3 bench/compare.py results.json --output report.txt

  # 5. Flag anything more than 50% over reference:
  python3 bench/compare.py results.json --threshold 50

  # 6. Emit comparison data as JSON for CI diffing:
  python3 bench/compare.py results.json --json-out deltas.json

Options
───────
  --markdown          GitHub-flavoured Markdown output
  --output FILE       Write report to FILE (default: stdout)
  --threshold N       Highlight entries where |overhead| > N%  (default: 200)
  --json-out FILE     Write structured comparison data to FILE
  --filter PATTERN    Only process benchmarks matching PATTERN (regex)
  --cpu               Use cpu_time instead of real_time (default: cpu_time)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field, asdict
from datetime import datetime
from pathlib import Path
from typing import Optional

# ─────────────────────────────────────────────────────────────────────────────
# § 0  Comparison pair definitions
#
# Each tuple is:
#   (subject_name, reference_name, display_label)
#
# subject_name / reference_name are the exact benchmark "name" fields from
# the JSON, or a regex that must fully match (anchored).
# ─────────────────────────────────────────────────────────────────────────────

# Metric: % overhead vs. native uint64_t  (should be small for Small ops)
NATIVE_PAIRS: list[tuple[str, str, str]] = [
    ("hydra/small_add",         "baseline/u64_add",  "small add"),
    ("hydra/small_mul",         "baseline/u64_mul",  "small mul"),
    ("hydra/small_sub",         "baseline/u64_add",  "small sub (vs add)"),
    ("hydra/widening_add",      "baseline/u64_add",  "widening add (vs native)"),
    ("hydra/widening_mul_128",  "baseline/u64_mul",  "widening mul 128 (vs native)"),
]

# Metric: % overhead vs. Boost.Multiprecision  (should be negative = faster)
# Only populated when boost/* benchmarks are present.
BOOST_PAIRS: list[tuple[str, str, str]] = [
    ("hydra/widening_add",        "boost/widening_add",    "widening add"),
    ("hydra/widening_mul_128",    "boost/widening_mul",    "widening mul 128"),
    ("hydra/medium_add",          "boost/large_add/128",   "medium add (vs large/128)"),
    ("hydra/medium_mul",          "boost/large_mul/128",   "medium mul (vs large/128)"),
    ("hydra/small_add",           "boost/small_add",       "small add"),
    ("hydra/small_mul",           "boost/small_mul",       "small mul"),
    ("hydra/large_add_cmp/128",   "boost/large_add/128",   "large add 128-bit"),
    ("hydra/large_add_cmp/256",   "boost/large_add/256",   "large add 256-bit"),
    ("hydra/large_add_cmp/512",   "boost/large_add/512",   "large add 512-bit"),
    ("hydra/large_mul_cmp/128",   "boost/large_mul/128",   "large mul 128-bit"),
    ("hydra/large_mul_cmp/256",   "boost/large_mul/256",   "large mul 256-bit"),
    ("hydra/large_mul_cmp/512",   "boost/large_mul/512",   "large mul 512-bit"),
]

# Standalone display groups — no reference, shown as cost tables.
# (prefix, section_title)
STANDALONE_GROUPS: list[tuple[str, str]] = [
    ("alloc/",  "Allocation costs"),
    ("copy/",   "Copy / Move costs"),
    ("chain/",  "Arithmetic chain throughput"),
]

# ─────────────────────────────────────────────────────────────────────────────
# § 1  Data loading
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class BenchResult:
    name:      str
    cpu_ns:    float
    real_ns:   float
    iters:     int
    time_unit: str   # as reported; we normalise to ns

    @classmethod
    def from_json(cls, obj: dict) -> "BenchResult":
        unit = obj.get("time_unit", "ns")
        scale = _unit_to_ns(unit)
        return cls(
            name      = obj["name"],
            cpu_ns    = obj.get("cpu_time", 0.0)  * scale,
            real_ns   = obj.get("real_time", 0.0) * scale,
            iters     = obj.get("iterations", 0),
            time_unit = unit,
        )

def _unit_to_ns(unit: str) -> float:
    return {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}.get(unit, 1.0)

def load_results(path: str, use_cpu: bool = True) -> dict[str, BenchResult]:
    with open(path) as f:
        data = json.load(f)
    out: dict[str, BenchResult] = {}
    for obj in data.get("benchmarks", []):
        # Skip aggregate rows (mean / stddev / median) — they have a
        # name suffix like "BenchmarkName_mean".
        if any(obj["name"].endswith(s) for s in ("_mean", "_median", "_stddev", "_cv")):
            continue
        r = BenchResult.from_json(obj)
        out[r.name] = r
    return out

def extract_context(path: str) -> dict:
    with open(path) as f:
        data = json.load(f)
    return data.get("context", {})

# ─────────────────────────────────────────────────────────────────────────────
# § 2  Formatting helpers
# ─────────────────────────────────────────────────────────────────────────────

def fmt_time(ns: float) -> str:
    """Human-readable time, auto-scaled."""
    if ns <= 0:
        return "—"
    if ns < 1:
        return f"{ns*1000:.1f} ps"
    if ns < 1_000:
        return f"{ns:.2f} ns"
    if ns < 1_000_000:
        return f"{ns/1_000:.2f} µs"
    return f"{ns/1_000_000:.2f} ms"

def pct_delta(subject: float, ref: float) -> tuple[float, str]:
    """Returns (raw_pct, formatted_string). Positive = subject is slower."""
    if ref <= 0:
        return float("nan"), "N/A"
    delta = (subject / ref - 1.0) * 100.0
    sign  = "+" if delta >= 0 else ""
    return delta, f"{sign}{delta:.1f}%"

def ratio(subject: float, ref: float) -> str:
    if ref <= 0:
        return "N/A"
    r = subject / ref
    return f"{r:.2f}×"

# ─────────────────────────────────────────────────────────────────────────────
# § 3  Report builder
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class ComparisonRow:
    label:      str
    subject_ns: float
    ref_ns:     float
    ref_name:   str
    delta_pct:  float
    delta_str:  str

@dataclass
class ComparisonSection:
    title:    str
    subtitle: str
    rows:     list[ComparisonRow] = field(default_factory=list)
    missing:  list[str]          = field(default_factory=list)

@dataclass
class StandaloneRow:
    label:  str
    params: dict[str, float]   # param_value → cpu_ns (or "" for unparam)

@dataclass
class StandaloneSection:
    title: str
    rows:  list[StandaloneRow] = field(default_factory=list)
    params: list[str]          = field(default_factory=list)  # ordered param values

# ─────────────────────────────────────────────────────────────────────────────
# § 4  Analysis
# ─────────────────────────────────────────────────────────────────────────────

def _lookup(results: dict[str, BenchResult], name: str) -> Optional[BenchResult]:
    return results.get(name)

def build_comparison_section(
    results:   dict[str, BenchResult],
    pairs:     list[tuple[str, str, str]],
    title:     str,
    subtitle:  str,
    use_cpu:   bool = True,
) -> ComparisonSection:
    section = ComparisonSection(title=title, subtitle=subtitle)
    for subj_name, ref_name, label in pairs:
        subj = _lookup(results, subj_name)
        ref  = _lookup(results, ref_name)
        if subj is None or ref is None:
            section.missing.append(f"{label} (missing: "
                + (f"{subj_name}" if subj is None else "")
                + (" + " if subj is None and ref is None else "")
                + (f"{ref_name}" if ref is None else "")
                + ")")
            continue
        s_ns = subj.cpu_ns if use_cpu else subj.real_ns
        r_ns = ref.cpu_ns  if use_cpu else ref.real_ns
        raw, ds = pct_delta(s_ns, r_ns)
        section.rows.append(ComparisonRow(
            label      = label,
            subject_ns = s_ns,
            ref_ns     = r_ns,
            ref_name   = ref_name,
            delta_pct  = raw,
            delta_str  = ds,
        ))
    return section

def build_standalone_section(
    results: dict[str, BenchResult],
    prefix:  str,
    title:   str,
    use_cpu: bool = True,
) -> StandaloneSection:
    section = StandaloneSection(title=title)
    # Group by (base_name → {param: time})
    groups: dict[str, dict[str, float]] = {}
    param_set: set[str] = set()

    for name, res in results.items():
        if not name.startswith(prefix):
            continue
        ns = res.cpu_ns if use_cpu else res.real_ns
        # Strip the family prefix, then check for a trailing /param
        rest   = name[len(prefix):]      # e.g. "from_limbs/16"
        parts  = rest.split("/")
        base   = parts[0]               # "from_limbs"
        param  = parts[1] if len(parts) > 1 else ""

        if base not in groups:
            groups[base] = {}
        groups[base][param] = ns
        param_set.add(param)

    # Sort params numerically where possible, else lexicographically.
    def sort_key(p: str) -> tuple:
        try:    return (0, int(p))
        except: return (1, p)

    section.params = sorted(param_set, key=sort_key)

    for base in sorted(groups):
        section.rows.append(StandaloneRow(
            label  = prefix + base,
            params = groups[base],
        ))
    return section

# ─────────────────────────────────────────────────────────────────────────────
# § 5  Rendering  (terminal and markdown)
# ─────────────────────────────────────────────────────────────────────────────

ANSI_RESET  = "\033[0m"
ANSI_GREEN  = "\033[92m"
ANSI_YELLOW = "\033[93m"
ANSI_RED    = "\033[91m"
ANSI_BOLD   = "\033[1m"
ANSI_DIM    = "\033[2m"

def _colour(s: str, pct: float, threshold: int, markdown: bool) -> str:
    if markdown:
        return s
    if abs(pct) < 10:
        return f"{ANSI_GREEN}{s}{ANSI_RESET}"
    if abs(pct) < threshold:
        return s
    return f"{ANSI_RED}{s}{ANSI_RESET}"

def render_comparison_section(
    sec:       ComparisonSection,
    markdown:  bool,
    threshold: int,
    use_cpu:   bool,
) -> list[str]:
    lines: list[str] = []
    metric = "cpu_time" if use_cpu else "real_time"

    if markdown:
        lines.append(f"### {sec.title}")
        lines.append(f"*{sec.subtitle} · metric: `{metric}`*")
        lines.append("")
        if not sec.rows:
            if sec.missing:
                lines.append(f"*Benchmarks not present in results: "
                             f"{', '.join(sec.missing)}*")
            return lines
        # Column widths
        lines.append("| Operation | Subject (ns) | Reference (ns) | Ratio | Δ |")
        lines.append("|-----------|-------------|----------------|-------|---|")
        for r in sec.rows:
            flag = " ⚠" if r.delta_pct > threshold else ""
            lines.append(
                f"| {r.label} "
                f"| {fmt_time(r.subject_ns)} "
                f"| {fmt_time(r.ref_ns)} `{r.ref_name}` "
                f"| {ratio(r.subject_ns, r.ref_ns)} "
                f"| {r.delta_str}{flag} |"
            )
    else:
        # Terminal
        W = 76
        lines.append(f"\n{'─' * 4} {sec.title} {'─' * max(0, W - 6 - len(sec.title))}")
        lines.append(f"  {ANSI_DIM}{sec.subtitle} · {metric}{ANSI_RESET}")
        lines.append("")
        if not sec.rows:
            if sec.missing:
                lines.append(f"  (not available — missing benchmarks)")
                for m in sec.missing:
                    lines.append(f"    • {m}")
            return lines
        # Header
        col = [32, 11, 11, 7, 8]
        header = (f"  {'Operation':<{col[0]}}  {'subject':>{col[1]}}"
                  f"  {'reference':>{col[2]}}  {'ratio':>{col[3]}}"
                  f"  {'Δ':>{col[4]}}")
        lines.append(f"{ANSI_BOLD}{header}{ANSI_RESET}")
        lines.append("  " + "─" * (sum(col) + 2 * (len(col) - 1)))
        for r in sec.rows:
            flag     = "  ⚠" if r.delta_pct >  threshold else \
                       "  ✓" if r.delta_pct < -10         else "   "
            dstr     = _colour(f"{r.delta_str:>{col[4]}}", r.delta_pct, threshold, markdown)
            lines.append(
                f"  {r.label:<{col[0]}}"
                f"  {fmt_time(r.subject_ns):>{col[1]}}"
                f"  {fmt_time(r.ref_ns):>{col[2]}}"
                f"  {ratio(r.subject_ns, r.ref_ns):>{col[3]}}"
                f"  {dstr}{flag}"
            )

    if sec.missing:
        lines.append("")
        skipped = f"  (skipped {len(sec.missing)} pair(s) — benchmarks not in results)"
        lines.append(f"{ANSI_DIM}{skipped}{ANSI_RESET}" if not markdown else skipped)
    return lines

def render_standalone_section(
    sec:      StandaloneSection,
    markdown: bool,
) -> list[str]:
    lines: list[str] = []
    if not sec.rows:
        return lines

    param_labels = [p for p in sec.params if p]   # numeric/named params only

    # Split rows into parameterised (has at least one non-"" param) and flat.
    param_rows = [r for r in sec.rows if any(k != "" for k in r.params)]
    flat_rows  = [r for r in sec.rows if all(k == "" for k in r.params)]

    W = 76
    if markdown:
        lines.append(f"### {sec.title}")
        lines.append("")
        if param_rows and param_labels:
            param_cols = " | ".join(f"{p} limbs" for p in param_labels)
            lines.append(f"| Benchmark | {param_cols} |")
            lines.append("|-----------|" + "|".join("-" * 12 for _ in param_labels) + "|")
            for row in param_rows:
                cells = " | ".join(
                    fmt_time(row.params[p]) if row.params.get(p) else "—"
                    for p in param_labels
                )
                lines.append(f"| `{row.label}` | {cells} |")
            if flat_rows:
                lines.append("")
        if flat_rows:
            lines.append("| Benchmark | Time |")
            lines.append("|-----------|------|")
            for row in flat_rows:
                t = row.params.get("", 0)
                lines.append(f"| `{row.label}` | {fmt_time(t)} |")
    else:
        lines.append(f"\n{'─' * 4} {sec.title} {'─' * max(0, W - 6 - len(sec.title))}")
        lines.append("")
        if param_rows and param_labels:
            col0 = max(len(r.label) for r in param_rows) + 2
            colw = 11
            header = f"  {'Benchmark':<{col0}}" + "".join(
                f"  {(p + ' limbs'):>{colw}}" for p in param_labels
            )
            lines.append(f"{ANSI_BOLD}{header}{ANSI_RESET}")
            lines.append("  " + "─" * (col0 + (colw + 2) * len(param_labels)))
            for row in param_rows:
                cells = "".join(
                    f"  {fmt_time(row.params[p]):>{colw}}" if row.params.get(p)
                    else f"  {'—':>{colw}}"
                    for p in param_labels
                )
                lines.append(f"  {row.label:<{col0}}{cells}")
        if flat_rows:
            if param_rows:
                lines.append("")
            col0 = max(len(r.label) for r in flat_rows) + 2
            lines.append(f"{ANSI_BOLD}  {'Benchmark':<{col0}}  {'Time':>10}{ANSI_RESET}")
            lines.append("  " + "─" * (col0 + 12))
            for row in flat_rows:
                t = row.params.get("", 0)
                lines.append(f"  {row.label:<{col0}}  {fmt_time(t):>10}")
    return lines

def render_header(
    path:      str,
    context:   dict,
    markdown:  bool,
    use_cpu:   bool,
) -> list[str]:
    ts      = datetime.now().strftime("%Y-%m-%d %H:%M")
    machine = context.get("host_name", context.get("executable", Path(path).stem))
    num_cpu = context.get("num_cpus", "?")
    mhz     = context.get("mhz_per_cpu", "?")
    build   = context.get("library_build_type", "")

    if markdown:
        return [
            "# Hydra Benchmark Report",
            "",
            f"Generated: `{ts}`  "
            f"Machine: `{machine}`  "
            f"CPUs: `{num_cpu} × {mhz} MHz`  "
            f"Build: `{build}`",
            "",
            f"> Source: `{Path(path).name}`  "
            f"Metric: `{'cpu_time' if use_cpu else 'real_time'}`",
            "",
        ]
    else:
        border = "═" * 76
        return [
            f"\n{ANSI_BOLD}{border}",
            "  Hydra Benchmark Comparison Report",
            f"  Generated: {ts}   Machine: {machine}",
            f"  CPUs: {num_cpu} × {mhz} MHz   Build: {build}",
            f"  Source: {Path(path).name}",
            f"{border}{ANSI_RESET}",
        ]

def render_footer(missing_all: list[str], markdown: bool) -> list[str]:
    if not missing_all:
        return []
    if markdown:
        return ["", "---", "", "**Note:** Some comparison pairs were skipped.",
                "Run with `--benchmark_filter=all` or enable Boost benchmarks."]
    return [f"\n{ANSI_DIM}{'─'*76}",
            "  Some comparison pairs were skipped (benchmarks not in results).",
            f"  Run hydra_bench with --benchmark_filter=all, or enable Boost.{ANSI_RESET}"]

# ─────────────────────────────────────────────────────────────────────────────
# § 6  JSON comparison output
# ─────────────────────────────────────────────────────────────────────────────

def build_json_output(
    native_sec: ComparisonSection,
    boost_sec:  ComparisonSection,
    standalone: list[StandaloneSection],
    context:    dict,
    path:       str,
) -> dict:
    def serialise_comparison(sec: ComparisonSection) -> dict:
        return {
            "title": sec.title,
            "rows": [
                {
                    "label":      r.label,
                    "subject_ns": r.subject_ns,
                    "ref_ns":     r.ref_ns,
                    "ref_name":   r.ref_name,
                    "delta_pct":  round(r.delta_pct, 2),
                    "delta_str":  r.delta_str,
                    "ratio":      round(r.subject_ns / r.ref_ns, 4) if r.ref_ns else None,
                }
                for r in sec.rows
            ],
            "missing": sec.missing,
        }

    return {
        "generated":  datetime.now().isoformat(),
        "source":     str(Path(path).resolve()),
        "context":    context,
        "vs_native":  serialise_comparison(native_sec),
        "vs_boost":   serialise_comparison(boost_sec),
        "standalone": [
            {
                "title":  s.title,
                "params": s.params,
                "rows": [
                    {"label": r.label, "times": r.params}
                    for r in s.rows
                ],
            }
            for s in standalone
        ],
    }

# ─────────────────────────────────────────────────────────────────────────────
# § 7  Main
# ─────────────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Hydra benchmark comparison tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("results",       help="Path to Google Benchmark JSON output")
    parser.add_argument("--markdown",    action="store_true",
                        help="Emit GitHub-flavoured Markdown tables")
    parser.add_argument("--output",      metavar="FILE",
                        help="Write report to FILE (default: stdout)")
    parser.add_argument("--threshold",   type=int, default=200,
                        help="Highlight entries with |Δ| > N%% (default: 200)")
    parser.add_argument("--json-out",    metavar="FILE",
                        help="Write comparison data as JSON to FILE")
    parser.add_argument("--filter",      metavar="PATTERN",
                        help="Only process benchmarks matching PATTERN (regex)")
    parser.add_argument("--real-time",   action="store_true",
                        help="Use real_time instead of cpu_time (default: cpu_time)")
    args = parser.parse_args()

    use_cpu = not args.real_time

    # ── Load ──────────────────────────────────────────────────────────────────
    try:
        results = load_results(args.results, use_cpu)
        context = extract_context(args.results)
    except FileNotFoundError:
        print(f"error: file not found: {args.results}", file=sys.stderr)
        return 1
    except json.JSONDecodeError as e:
        print(f"error: invalid JSON: {e}", file=sys.stderr)
        return 1

    if args.filter:
        pat = re.compile(args.filter)
        results = {k: v for k, v in results.items() if pat.search(k)}

    if not results:
        print("error: no benchmarks found (check --filter or file path)", file=sys.stderr)
        return 1

    # ── Build sections ────────────────────────────────────────────────────────
    native_sec = build_comparison_section(
        results, NATIVE_PAIRS,
        title    = "Small operations vs. native uint64_t",
        subtitle = "Lower Δ is better. Goal: < 2× native for small ops.",
        use_cpu  = use_cpu,
    )

    boost_sec = build_comparison_section(
        results, BOOST_PAIRS,
        title    = "Medium / Large operations vs. Boost.Multiprecision",
        subtitle = "Negative Δ means Hydra is faster than Boost.",
        use_cpu  = use_cpu,
    )

    standalone_sections = [
        build_standalone_section(results, prefix, title, use_cpu)
        for prefix, title in STANDALONE_GROUPS
    ]

    # ── Render ────────────────────────────────────────────────────────────────
    lines: list[str] = []
    lines += render_header(args.results, context, args.markdown, use_cpu)
    lines += render_comparison_section(native_sec,  args.markdown, args.threshold, use_cpu)
    lines.append("")
    lines += render_comparison_section(boost_sec,   args.markdown, args.threshold, use_cpu)
    for s in standalone_sections:
        lines.append("")
        lines += render_standalone_section(s, args.markdown)
    all_missing = native_sec.missing + boost_sec.missing
    lines += render_footer(all_missing, args.markdown)
    lines.append("")

    out_text = "\n".join(lines)

    # ── Output ────────────────────────────────────────────────────────────────
    if args.output:
        # Strip ANSI codes when writing to a file (unless markdown).
        if not args.markdown:
            out_text = re.sub(r"\033\[[0-9;]*m", "", out_text)
        Path(args.output).write_text(out_text)
        print(f"Report written to {args.output}", file=sys.stderr)
    else:
        # Only strip ANSI if stdout is not a tty.
        if not sys.stdout.isatty() and not args.markdown:
            out_text = re.sub(r"\033\[[0-9;]*m", "", out_text)
        print(out_text)

    # ── JSON comparison output ────────────────────────────────────────────────
    if args.json_out:
        data = build_json_output(native_sec, boost_sec, standalone_sections,
                                 context, args.results)
        Path(args.json_out).write_text(json.dumps(data, indent=2))
        print(f"Comparison JSON written to {args.json_out}", file=sys.stderr)

    return 0

if __name__ == "__main__":
    sys.exit(main())
