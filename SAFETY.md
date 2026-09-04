# Rust safety invariants

VodLink's first-party Rust is safe Rust only. These rules are build policy, not review suggestions:

- `src/lib.rs` and `src/main.rs` start with `#![forbid(unsafe_code)]`.
- Cargo forbids `unsafe_code`, `unused_unsafe`, unsafe operations inside unsafe functions, pointer transmute lint escapes, undocumented unsafe blocks, and lint-allow attributes.
- `.cargo/config.toml` passes `-Funsafe-code` to every first-party Rust compilation.
- `scripts/check-rust-safety.py` rejects first-party `unsafe`, raw-pointer/FFI escape hatches, lint overrides, and any first-party `build.rs`.
- Miri runs over the no-OBS first-party core. OBS is isolated behind the safe, pinned `libobs-rs` API and tested separately on Windows.
- The validation workflow has `contents: read` and checkout credentials are not persisted. It cannot push changes or create pull requests.

Agent tooling should treat `Cargo.toml`, `.cargo/config.toml`, crate roots, `scripts/check-rust-safety.py`, and `.github/workflows/rust-safety.yml` as protected policy boundaries. Any change to one of these files must preserve the checks above and is expected to fail CI if it weakens them.
