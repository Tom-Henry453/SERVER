# What changed since the last commit

Base commit: `ecd67f8` — "Client coonects > server accepts and createsa new thread for that client"

This pass turned the server from "compiles once, works on the happy path" into a security-hardened build with typed error handling.

## Build

- Bumped to **C++23** (`g++ -std=c++23 -O2 -pthread -I. SERVER.cpp -o server_fixed`) — required for `std::expected`, used throughout the new request-parsing code for typed error codes instead of silent `-1`/`nullopt` returns.
- Consolidated scattered `#define` constants into `configs.hh` (request size caps, timeouts, MIME/status enums, `utils::trim`).

## Refactor

- Introduced `HTTP_request.hh` — `http_request` class holds parsed request data (`method`, `path`, `version`, `headers`, `body`) plus two `static` factory methods, `receive_request` and `parse_request`, both returning `std::expected<T, int>`.
- Rewrote request-line parsing: replaced a `stringstream >>` extraction (silently tolerant of malformed whitespace, truncated fields, extra tokens) with explicit two-`find(' ')` parsing that rejects anything not in strict `METHOD SP PATH SP VERSION` form.

## Security fixes

| # | Fix | What it stops |
|---|-----|----------------|
| A1 | `listen()` backlog 5 → 128 | Connection bursts refused outright under trivial concurrent load |
| A2 | Method allowlist — GET only, else `404` | Any HTTP verb (`DELETE`, `PUT`, etc.) reaching the file-serving code path |
| A3 | Served-file size cap (10 MB) in `open_file` | Unbounded RAM read of arbitrarily large files under `Resources/` |
| A4 | Request body size cap (1 MB) in `receive_request` | OOM via a spoofed `Content-Length` header |
| A5 | `SO_RCVTIMEO` / `SO_SNDTIMEO` on each accepted socket | Slowloris — a connection trickling bytes to hold a thread open indefinitely. Code written and compiles; not yet load-tested. |

## Bugs fixed along the way

- `recv() == -1` case (kernel error/timeout) was falling through to `string::append` with an implicit huge length — now every negative/zero return from `recv` is treated as a hard failure.
- Several compile-breaking typos introduced mid-refactor: dangling `||`/`else`, a multi-char `'\r\n'` literal passed where a single `char` delimiter was required, `path == '.'` (char) vs `path == "."` (string) type mismatch, a missing `#include <expected>`.
- `configs.hh` had a missing semicolon and a `trim` helper with an early-return bug, both fixed.
