# Contributing to TextSearch

Thanks for your interest! A few conventions keep this codebase small and
auditable.

## Build & test

```bash
cmake -G "MinGW Makefiles" -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires MinGW-w64 GCC ≥ 15 (UCRT). Without CMake, direct g++ commands are in
[README.zh-CN.md](README.zh-CN.md) («无 CMake 时的直接编译»).

**Every PR must keep `tests` green** (they run headless — no GUI needed).

## Code conventions

- **Layering**: `core/` is platform-neutral logic (plus `core/port/` for OS
  specifics and `core/i18n.cpp` for UI strings); `gui/` is Win32-only;
  `cli/` depends on `core/` only. Don't cross wires: GUI logic stays in
  `gui/`, search logic stays in `core/`.
- **Zero third-party dependencies** — including "temporarily". If you need a
  parser, write a small one (see `core/json.h`, `core/inifile.cpp` for the
  house style).
- **UI strings** go through `i18n::tr("id")` / `i18n::trf(...)` with both a
  zh and an en column in `core/i18n.cpp`. A missing entry falls back to the
  id itself, which is visible — add the entry.
- **Tunable constants** belong in the INI (`core/config.cpp`), not as
  literals — every user-visible knob has a documented default there.
- **Correctness changes** (search, index, encoding) need a regression test,
  and ideally the "reverse-verify" treatment: temporarily revert your fix
  and show the new test fails.
- **Privacy is a hard guarantee**: search keywords are never persisted —
  don't add code paths that write them anywhere.

## Commits

Conventional style (`fix(scope): …`, `feat(scope): …`), one logical change
per commit. Chinese or English both fine.
