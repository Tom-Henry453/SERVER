# Raw Server

A multi-threaded HTTP/1.1 file server written from scratch in C++23 — no framework, just POSIX sockets. It listens on port 4221 and serves static files from `Resources/`.

This is a learning project: every fix is pair-programmed (I write the code, Claude explains the concept and reviews), and every security issue found in an audit gets fixed and documented rather than patched silently.

## Build & run

```
g++ -std=c++23 -O2 -pthread -I. SERVER.cpp -o server_fixed
./server_fixed
```

Then `curl http://localhost:4221/`.

## Status

Hardening the server against a set of DoS/crash issues found in a security audit: unbounded threads, no timeouts, unbounded body/file reads, missing method validation, tiny accept backlog. See `FIXED.md` for what changed most recently, or `PROGRESS.md` for the full checklist.

## Design constraints (deliberate, for now)

- **GET only.** Every other HTTP method is rejected.
- **No TLS.** HTTPS is planned as the very last piece of work.

## Why build an HTTP server by hand

To actually understand what a framework hides: TCP accept/backlog semantics, blocking I/O and why it needs timeouts, and where naive request parsing breaks under malformed or hostile input.
