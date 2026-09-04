#!/usr/bin/env python3
"""Fail closed when VodLink's first-party Rust safety invariants are weakened."""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CARGO = ROOT / "Cargo.toml"
CRATE_ROOTS = (ROOT / "src/lib.rs", ROOT / "src/main.rs")

FORBIDDEN_SOURCE_PATTERNS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"\bunsafe\b"), "unsafe token"),
    (re.compile(r"#!\s*\[\s*(?:allow|warn)\s*\("), "crate-level lint override"),
    (re.compile(r"#\s*\[\s*(?:allow|warn)\s*\("), "item-level lint override"),
    (re.compile(r"\*\s*(?:mut|const)\b"), "raw pointer type"),
    (re.compile(r"\b(?:addr_of|addr_of_mut|NonNull|transmute|from_raw|into_raw)\b"), "raw-memory escape hatch"),
    (re.compile(r"extern\s*\"C\""), "first-party C FFI boundary"),
)


def fail(message: str) -> None:
    print(f"rust-safety: {message}", file=sys.stderr)
    raise SystemExit(1)


def require_lint(table: dict[str, object], name: str, expected: str = "forbid") -> None:
    value = table.get(name)
    if value != expected:
        fail(f"Cargo lint {name!r} must be {expected!r}, got {value!r}")


def main() -> None:
    if (ROOT / "build.rs").exists():
        fail("first-party build.rs is forbidden; native build escape hatches are not permitted")

    cargo = tomllib.loads(CARGO.read_text(encoding="utf-8"))
    lints = cargo.get("lints", {})
    rust_lints = lints.get("rust", {}) if isinstance(lints, dict) else {}
    clippy_lints = lints.get("clippy", {}) if isinstance(lints, dict) else {}
    if not isinstance(rust_lints, dict) or not isinstance(clippy_lints, dict):
        fail("Cargo lint tables are missing")

    require_lint(rust_lints, "unsafe_code")
    require_lint(rust_lints, "unused_unsafe")
    require_lint(rust_lints, "unsafe_op_in_unsafe_fn")
    require_lint(clippy_lints, "transmute_ptr_to_ptr")
    require_lint(clippy_lints, "undocumented_unsafe_blocks")
    require_lint(clippy_lints, "allow_attributes")
    require_lint(clippy_lints, "allow_attributes_without_reason")

    config = (ROOT / ".cargo/config.toml").read_text(encoding="utf-8")
    if "-Funsafe-code" not in config:
        fail(".cargo/config.toml must force -Funsafe-code")

    for crate_root in CRATE_ROOTS:
        first_nonempty = next(
            (line.strip() for line in crate_root.read_text(encoding="utf-8").splitlines() if line.strip()),
            "",
        )
        if first_nonempty != "#![forbid(unsafe_code)]":
            fail(f"{crate_root.relative_to(ROOT)} must begin with #![forbid(unsafe_code)]")

    violations: list[str] = []
    for source in sorted((ROOT / "src").rglob("*.rs")):
        text = source.read_text(encoding="utf-8")
        for pattern, description in FORBIDDEN_SOURCE_PATTERNS:
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                violations.append(f"{source.relative_to(ROOT)}:{line}: {description}")

    if violations:
        fail("first-party Rust policy violations:\n  " + "\n  ".join(violations))

    print("rust-safety: first-party invariants verified")


if __name__ == "__main__":
    main()
