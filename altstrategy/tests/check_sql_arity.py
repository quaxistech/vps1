#!/usr/bin/env python3
"""Every parameterised INSERT must supply exactly as many VALUES expressions as
it names columns.

This exists because a 67-column INSERT in decision_store.cpp shipped with 68
placeholders. The whole unit suite passed -- nothing in it executes real SQL --
and the failure surfaced only as `db_write_failed` in production, where trade
proposals silently stopped being archived while the engine kept running.

A string assembled by hand and validated only by the database is checked here
instead, at build time.
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
failures = []


def balanced(text: str, open_at: int) -> str:
    """Contents of the parenthesis group that starts at `open_at`."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_at + 1 : i]
    return ""


def split_top_level(text: str) -> list[str]:
    """Split on commas that are not inside parentheses."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [c for c in out if c.strip()]


for src in sorted(root.glob("src/**/*.cpp")):
    # Join adjacent C++ string literals so a statement split across lines reads
    # as one string, exactly as the compiler sees it.
    joined = re.sub(r'"\s*\n\s*"', "", src.read_text())
    for m in re.finditer(r"INSERT INTO ([\w.]+)\s*\(", joined):
        table = m.group(1)
        cols_open = joined.index("(", m.end() - 1)
        cols = split_top_level(balanced(joined, cols_open))
        # Bound the search to THIS statement: stop at the next INSERT, and skip
        # INSERT ... SELECT forms, whose arity the planner checks for us.
        after_cols = joined.index(")", cols_open) + 1
        nxt = joined.find("INSERT INTO", after_cols)
        tail = joined[cols_open : nxt if nxt != -1 else len(joined)]
        head = joined[after_cols : nxt if nxt != -1 else len(joined)]
        vm = re.search(r"\bVALUES\s*\(", tail)
        if not vm:
            continue
        if re.search(r"\bSELECT\b", head[: vm.start() - (after_cols - cols_open)] or ""):
            continue
        vals = split_top_level(balanced(tail, tail.index("(", vm.end() - 1)))
        nums = sorted({int(n) for n in re.findall(r"\$(\d+)", " ".join(vals))})
        if len(cols) != len(vals):
            failures.append(
                f"{src.relative_to(root)}: INSERT INTO {table} names {len(cols)} columns "
                f"but supplies {len(vals)} VALUES expressions"
            )
        elif nums and nums != list(range(1, len(nums) + 1)):
            failures.append(
                f"{src.relative_to(root)}: INSERT INTO {table} placeholders are not "
                f"contiguous from $1 (highest ${nums[-1]}, count {len(nums)})"
            )

if failures:
    print("SQL arity check FAILED:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("SQL arity check passed")
