# Bugs found and fixed

A record of the defects that mattered in this server: what the symptom was, what
the root cause turned out to be, how it was fixed, and how the fix was proven.
Typos and syntax errors are left out. Most significant first.

Every number below was measured on an Apple M5 (10 cores, macOS) over loopback
with [Fortio](https://github.com/fortio/fortio), keep-alive off.

---

## 1. One disconnecting client killed the whole server (SIGPIPE)

**Symptom.** Under a 200-connection load test the server process vanished
mid-run. No error message, no log line. Exit status 141.

**Root cause.** 141 is 128 + 13, meaning the process was terminated by signal 13,
`SIGPIPE`. The kernel raises it when a process writes to a socket whose peer is
gone, and its default action is to kill the process.

The code already tried to prevent this with the per-socket option
`SO_NOSIGPIPE`, but nobody checked whether setting it worked. A stack trace
captured in a signal handler placed the kill at the `send()` of an ordinary
200 response, and a standalone experiment isolated the one ordering that
defeats the option:

| Client behaviour | `setsockopt` | `recv` | SIGPIPE on `send` |
|---|---|---|---|
| closes after the option is set | ok | request bytes | no |
| resets after the option is set | ok | request bytes | no |
| closes before `accept()` | ok | request bytes | no |
| **resets before `accept()`** | **fails, `EINVAL`** | **request bytes** | **yes** |

With 200 clients and 4 workers, most connections wait in a queue. If a client
resets while waiting, the option can no longer be applied to that socket, yet
the request bytes that arrived earlier are still readable. The server parsed a
valid request from a client that no longer existed, then wrote to it unprotected.

**Fix.** `signal(SIGPIPE, SIG_IGN)` once at start-up. This is process-wide, so it
cannot fail per connection, and it behaves the same on Linux and macOS. A write
to a dead peer now returns `-1` with `errno == EPIPE` and is handled as a normal
error. The macOS-only `SO_NOSIGPIPE` code was removed.

**Proof.** The same test at 200, 400 and 800 connections now completes with the
server alive, and the log shows the previously fatal case being handled:
108 "client disconnected" entries in one 30-second run.

**Takeaway.** A server cannot know a client is still there; it can only find out
by doing I/O. So the design rule is not "detect departure first" but "every
operation on a client socket may fail, and failure means close and move on".

---

## 2. Responses were sent with one `send()` call and the result was thrown away

This one never failed in a test, which is exactly why it sat there for so long.
I only looked at it properly after the SIGPIPE fix, because that fix changed
what a write to a dead client does: it used to kill the process, now `send()`
returns `-1`. And nothing was reading that return value.

The old code, in four places:

```cpp
send(client_fd, response.c_str(), response.size(), 0);
close(client_fd);
```

There are three separate things wrong with those two lines.

**`send()` does not promise to send everything.** It copies bytes from my
buffer into the kernel's send buffer for that socket and returns how many it
copied. On a blocking socket it normally waits until all of them fit, but not
always. If a signal interrupts it part-way, or the socket has a send timeout
and the timeout runs out while it is waiting for space, it returns the count so
far. This server sets a 2-second `SO_SNDTIMEO` on every client socket, so the
second case was reachable by design: a client that reads slowly fills the
buffer, `send()` waits 2 seconds, then returns a partial count. The old code
ignored the count and closed the socket. The client would have received headers
saying `Content-Length: 5242880` followed by a body that stops early.

**`send()` can fail, and `-1` was ignored.** `EPIPE` or `ECONNRESET` when the
client has gone, `EINTR` when a signal arrives before any byte was copied,
`EAGAIN` when the send timeout expires with nothing copied. All of them looked
the same as success. No log line, no way to tell a served request from a
failed one.

**There was no limit on the response as a whole.** The 2-second timeout is per
call. It says nothing about how long the entire response may take.

### The fix

Every response now goes through `try_send_all`, which is a loop around `send()`:

```cpp
while (offset < size) {
    if (deadline passed)            return unexpected(12);   // whole-response timeout
    n = send(fd, buf + offset, size - offset, 0);
    if (n > 0)                      { offset += n; continue; }  // short write: carry on from here
    if (errno == EINTR || errno == EAGAIN) continue;            // nothing lost, try again
    return unexpected(13);                                      // peer is gone
}
```

- A short write is no longer an error or a silent loss. The loop moves `offset`
  forward and sends the rest.
- `EINTR` and `EAGAIN` mean "nothing went wrong, nothing was sent", so they retry.
- Any other error ends the response and is reported to the caller, which logs it.
- Two timers now work together. `SO_SNDTIMEO` (2 s) stops a single `send()` from
  blocking forever. The deadline (8 s) caps the whole response, so a client that
  accepts one byte every 1.9 seconds cannot hold a worker thread indefinitely.

The four call sites share one helper, `send_response`, so the logging and the
error handling live in one place.

### What I could and could not prove

- A 5 MB file arrives complete: 5,242,880 of 5,242,880 bytes.
- The failure path is exercised for real. Under load the log fills with
  "client disconnected" entries from `send_response` (108 in one 30-second run),
  each one a write that used to go unnoticed.
- I could not force a short write on loopback. I tried a client throttled to
  50 KB/s, and the kernel simply buffered the response on the server's behalf.
  So the loop is correct by reading it, and the short-write branch is not yet
  covered by a test.

One thing that test did teach me: `send()` returning success means the kernel
has taken the bytes, not that the client has them. The server had finished and
closed the connection while the slow client was still reading.

---

## 3. Connections refused under load: the listen backlog was 128, not 4096

**Symptom.** Above about 200 concurrent connections, 0.1% to 0.8% of requests
failed with "connection reset by peer", although the server was healthy.

**Root cause.** The code calls `listen(fd, 4096)`, but the kernel silently clamps
the backlog to `kern.ipc.somaxconn`, which is 128 on macOS. With keep-alive off,
every request is a new connection (about 27,000 per second here), so a short
burst overflows a 128-slot accept queue and the kernel refuses the excess.

This was confirmed by experiment, not assumed. The concurrency at which errors
first appear follows the backlog size:

| Backlog | 50 conns | 100 conns | 200 conns |
|---|---|---|---|
| 16 | 0.4% errors | 1.5% | 0.2% |
| 64 | 0 | 0.1% | 0.3% |
| 128 | 0 | 0 | 0.1% |

**Fix.** No code change was needed; the limit is the operating system's. Raising
it (`sudo sysctl -w kern.ipc.somaxconn=2048`) removed the errors completely:
0 failures at 200, 400 and 800 connections. On Linux 5.4 and later the default
cap is already 4096.

**Takeaway.** A benchmark error is not automatically a server bug. The setting is
recorded next to the benchmark results because they cannot be reproduced
without it.

---

## 4. Smaller fixes

| Defect | Risk | Fix |
|---|---|---|
| `Content-Length` was trusted | a 40-byte request claiming a 1 GB body bought a 1 GB allocation | validate the claim against a 1 MB cap before reading any body |
| Files were read into memory whole | one huge file in `Resources/`, multiplied by every concurrent request, exhausts memory | check the size first and refuse anything over 10 MB |
| Any HTTP method was served | `DELETE`, `POST` or a made-up verb all returned the file | default-deny: only `GET` is accepted |
| Body-read deadline measured from the wrong moment | the 8-second request limit was checked as `now - now` while reading a body, so it could never expire; a client dripping one byte every 2.9 s could hold a worker almost indefinitely | measure from the request start, so headers and body share one deadline. A client dripping a byte every 2 s is now cut off at about 10 s |
| `listen()` backlog was 5 | the sixth simultaneous client was refused before the server saw it | raised (now 4096, see item 3) |
| Path traversal | `GET /../../etc/passwd` | resolve the canonical path and refuse anything outside `Resources/` |

---

## Known issues, not yet fixed

Listed so the state of the code is not overstated.

- **File descriptor leak when the work queue is full.** A connection dropped
  because the queue is at capacity is never closed.
- **Shutdown race in the thread pool.** The stop flag is set without holding
  the queue mutex, so a worker can miss the wake-up.
