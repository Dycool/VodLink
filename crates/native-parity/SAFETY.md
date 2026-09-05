# Native parity safety boundary

This crate exists only where exact C++ VodLink parity requires direct operating-system APIs that do not have an adequate safe Rust abstraction.

The main `vodlink` crate remains `#![forbid(unsafe_code)]`. Unsafe Rust is not permitted there. This crate is intentionally small, Windows-only at runtime, and is audited as a compatibility boundary rather than general application logic.

## Approved parity responsibilities

1. Enumerate the same user-facing Windows processes as the C++ manual game picker: visible, titled, unowned top-level windows that are not tool windows, followed by PID-to-executable resolution.
2. Preserve Windows process integration details that require native APIs when a safe wrapper cannot reproduce them exactly.

Every unsafe block must have a nearby `SAFETY:` comment describing the pointer/handle lifetime or callback invariant that makes the operation valid. `transmute`, lint suppression, and unrelated native calls are forbidden by repository CI.
