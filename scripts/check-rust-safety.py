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
NATIVE_PARITY_ROOT = ROOT / "crates" / "native-parity"
NATIVE_PARITY_LIB = NATIVE_PARITY_ROOT / "src" / "lib.rs"

SAFE_SOURCE_PATTERNS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"\bunsafe\b"), "unsafe token"),
    (re.compile(r"#!\s*\[\s*(?:allow|warn)\s*\("), "crate-level lint override"),
    (re.compile(r"#\s*\[\s*(?:allow|warn)\s*\("), "item-level lint override"),
    (re.compile(r"\*\s*(?:mut|const)\b"), "raw pointer type"),
    (re.compile(r"\b(?:addr_of|addr_of_mut|NonNull|transmute|from_raw|into_raw)\b"), "raw-memory escape hatch"),
    (re.compile(r"extern\s*(?:\"C\"|\"system\")"), "first-party FFI boundary"),
)

NATIVE_PARITY_FORBIDDEN: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"#!\s*\[\s*(?:allow|warn)\s*\("), "crate-level lint override"),
    (re.compile(r"#\s*\[\s*(?:allow|warn)\s*\("), "item-level lint override"),
    (re.compile(r"\btransmute\b"), "transmute is forbidden even in native parity"),
)


def fail(message: str) -> None:
    print(f"rust-safety: {message}", file=sys.stderr)
    raise SystemExit(1)


def require_lint(table: dict[str, object], name: str, expected: str = "forbid") -> None:
    value = table.get(name)
    if value != expected:
        fail(f"Cargo lint {name!r} must be {expected!r}, got {value!r}")


def source_violations(source: Path, patterns: tuple[tuple[re.Pattern[str], str], ...]) -> list[str]:
    text = source.read_text(encoding="utf-8")
    violations: list[str] = []
    for pattern, description in patterns:
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            violations.append(f"{source.relative_to(ROOT)}:{line}: {description}")
    return violations


def verify_native_parity() -> list[str]:
    if not NATIVE_PARITY_ROOT.exists():
        return []
    if not NATIVE_PARITY_LIB.exists():
        return ["crates/native-parity: native parity crate must have src/lib.rs"]

    first_nonempty = next(
        (line.strip() for line in NATIVE_PARITY_LIB.read_text(encoding="utf-8").splitlines() if line.strip()),
        "",
    )
    if first_nonempty != "#![deny(unsafe_op_in_unsafe_fn)]":
        return ["crates/native-parity/src/lib.rs must begin with #![deny(unsafe_op_in_unsafe_fn)]"]

    safety_doc = NATIVE_PARITY_ROOT / "SAFETY.md"
    if not safety_doc.exists() or "parity" not in safety_doc.read_text(encoding="utf-8").lower():
        return ["crates/native-parity/SAFETY.md must document the parity justification"]

    violations: list[str] = []
    for source in sorted(NATIVE_PARITY_ROOT.rglob("*.rs")):
        text = source.read_text(encoding="utf-8")
        violations.extend(source_violations(source, NATIVE_PARITY_FORBIDDEN))
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if "unsafe {" not in line:
                continue
            context = "\n".join(lines[max(0, index - 3):index])
            if "SAFETY:" not in context:
                violations.append(
                    f"{source.relative_to(ROOT)}:{index + 1}: unsafe block requires a nearby SAFETY: justification"
                )
    return violations


def main() -> None:
    build_scripts = [path for path in ROOT.rglob("build.rs") if "target" not in path.parts]
    if build_scripts:
        fail("first-party build.rs is forbidden: " + ", ".join(str(path.relative_to(ROOT)) for path in build_scripts))

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

    for crate_root in CRATE_ROOTS:
        first_nonempty = next(
            (line.strip() for line in crate_root.read_text(encoding="utf-8").splitlines() if line.strip()),
            "",
        )
        if first_nonempty != "#![forbid(unsafe_code)]":
            fail(f"{crate_root.relative_to(ROOT)} must begin with #![forbid(unsafe_code)]")

    violations: list[str] = []
    for source in sorted(ROOT.rglob("*.rs")):
        if "target" in source.parts or source.is_relative_to(NATIVE_PARITY_ROOT):
            continue
        violations.extend(source_violations(source, SAFE_SOURCE_PATTERNS))
    violations.extend(verify_native_parity())

    if violations:
        fail("first-party Rust policy violations:\n  " + "\n  ".join(violations))

    print("rust-safety: safe first-party code verified; native exceptions are confined to crates/native-parity")


if __name__ == "__main__":
    main()
