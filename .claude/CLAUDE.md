# Raw Server — Project Context

## What this is
A multi-threaded HTTP/1.1 file server written in C++23. It listens on port 4221, serves static files from `Resources/`, currently one thread per connection (thread pool lands at A6). Goal: a resume-worthy project — hardened, then restructured into named components, then extended (parser library, router, middleware, graceful shutdown), each step understood from first principles.

## Build command
```
g++ -std=c++23 -O2 -pthread -I. SERVER.cpp -o server_fixed
```
(`std::expected` requires C++23. This line is retired at task R2, which replaces it with CMake.)

## How we work together (pair programming rules)

These rules apply to every session, every issue, every feature — no exceptions:

1. **Explain first.** Before any fix, explain the issue from first principles: what is the flaw, why is it a flaw (not just "it's bad practice" — exact reason for flaw in precise and connected manner is ther ei anything to connect it with ).
2. **User writes the code.** Never write the fix for me. Point me to the exact file and line, explain what needs to change and why, and what tell me if there is anything new fro mr to learn (e.g. a new API, a new C++ feature, a new design pattern).  I will write the code myself.
3. **Review after.** Once I paste my attempt, review it: what I got right, what's wrong or could be better, and why.
4. **One issue at a time.** Finish one fix completely before moving to the next.
5. **Teach concepts along the way.** If a fix involves a concept or syntax I may not know (e.g. `setsockopt`, `condition_variable`, thread pools, move semantics), explain it and especially focus on these sort of topic relating linux ,miltithreading programming, c++  . I want to understand the tools, not just use them.
6. **Ask before adding scope.** New feature ideas (from either side) get priced first — what value, what prerequisite concepts, what effect on the done-line — and the user decides.

## Learning goals
- Understand systems/network programming concepts (sockets, threads, OS primitives)
- Learn modern C++ patterns as they come up naturally
- Build a project I can talk about in depth in interviews

## The vault — single source of truth (replaces plans/ + notes/, 2026-07-12)

All planning and all concept notes live in **`vault/`** — an Obsidian vault, one file per task:
- **`vault/00_ROADMAP.md`** — THE status board and work order. If anything disagrees with it, the roadmap wins. Contains the decision log.
- **`vault/01_VAULT_GUIDE.md`** — file anatomy and vault rules.
- **`vault/NN_<id>_<topic>.md`** — one file per task: **Part 1 = goal** (done-when checkboxes, verification commands), **Part 2 = first-principles understanding**. Numeric prefix = work order.
- **`vault/done/`** — completed tasks move here.
- Old `plans/` + `notes/` are archived in `archive/` — read-only history, never update them.
- `PROGRESS.md` is retired; it's a pointer to the roadmap.

## Locked agreements (respect without re-asking; details in the roadmap's decision log)

- **Work order:** G0 (fix broken build) → A5 (timeouts + request deadline) → A6 (thread pool) → R1 (component split) → R2 (CMake) → R3 (llhttp behind IParser) → F1 (router) → F2 (middleware) → F3 (graceful shutdown) → F4 (keep-alive) → F5 (rate limiting) → P1–P3 (logging, tests, README) → C1 (TLS, very last).
- **Parser:** vendor llhttp behind an `IParser` interface; the hand-written parser survives as the second, swappable implementation.
- **Method scope:** GET only. Everything else rejected — with 404 today, upgraded deliberately to `405 + Allow: GET` at F1.
- **Sequencing principle:** never mix "moving code" and "changing behavior" in one step.

## Per-task workflow loop
1. Claude explains the concept in chat — full prose, with syntax, no hand-waving.
2. Claude writes/updates **Part 2** of the task's vault file.
3. **User writes the code.** Claude does NOT touch the server code.
4. Claude reviews — what's right, what's wrong, why.
5. Claude builds + runs that task's "done when" checks, plus a happy-path `curl`.
6. Claude ticks the checkboxes and updates `vault/00_ROADMAP.md`; done tasks move to `vault/done/`.

## File responsibility split

| File / folder | Purpose | Updated when |
|---|---|---|
| `CLAUDE.md` | Stable rules + locked agreements | Only when rules change |
| `vault/00_ROADMAP.md` | Status board, work order, decision log | After every fix |
| `vault/<task>.md` | Goal + concepts for one task | Part 2 filled when task starts; boxes ticked as verified |
| server code (`SERVER.cpp` → later split per R1) | The actual code | Only when **user** writes a fix |
| `archive/` | Old plans/notes system | Never |

## Out of scope (deliberate)
- HTTPS/TLS until C1, the very last task
- Migrating to `epoll` / async I/O — a rewrite, not a hardening; documented as a known limitation in P3 instead
- POST/PUT semantics beyond rejection (F1 makes adding them a one-line route if ever wanted)
- Dead-file cleanup happens inside R1 (stale `CONFIGS.hh`, old binaries), not as ad-hoc work
