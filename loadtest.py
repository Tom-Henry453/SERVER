#!/usr/bin/env python3
"""
loadtest.py - simple ramping load test for the Raw HTTP server.
This script increases the number of simultaneous clients and measures how long
the server takes to answer.
Latency means:
    connect -> send request -> read full response -> close connection

p95 latency means:
    95 percent of successful requests finished at this speed or faster.
"""
import asyncio
import time
import statistics
import argparse
import sys
import csv
import os
async def one_request(host: str, port: int, path: str, timeout: float) :
    """Send one HTTP request and measure how long it takes.
    Inputs:
        host: server address, for example "127.0.0.1"
        port: server port, for example 4221
        path: HTTP path to request, for example "/"
        timeout: maximum seconds this request may take
    What it does:
        Opens one TCP connection, sends one GET request, reads the full
        response, closes the connection, and measures the total time.
    Async idea:
        This is `async def`, so `await` pauses only this request while the
        network waits. Other requests can keep running on the same event loop.
    Output:
        (elapsed_seconds, ok, error_text)
        ok is True only when the server sends a non-empty response."""
    start = time.perf_counter()  # Start a high-precision timer for this request.
    try:
        request = (  # Build a small valid HTTP/1.1 request.
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
        ).encode()
        async def _do():
            """
            Do the network work for one request.
            Inputs:
                Uses host, port, and request from one_request().
            What it does:
                Connects, sends the request bytes, reads the response bytes,
                and closes the socket.
            Async idea:
                Each `await` waits for socket progress without blocking the
                whole load tester.
            Output:
                Number of response bytes received.
            """
            reader, w = await asyncio.open_connection(host, port)  # Connect without blocking other requests.
            try:
                w.write(request)  # Put request bytes into Python's socket buffer.
                await w.drain()  # Wait until the request bytes are actually sent.
                data = await reader.read()  # Wait until the server sends everything and closes.
                return len(data)  # Return size so empty responses can be detected.
            finally:
                w.close()  # Start closing our side of the connection.
                      
        try:
            n = await asyncio.wait_for(
                _do(), timeout=timeout
            )  # Stop this request if it hangs too long.
        except asyncio.TimeoutError:
            return time.perf_counter() - start, False, "timeout"
                
        elapsed = time.perf_counter() - start  # Convert start/end time into latency.

        if n <= 0:  # Empty response means the server did not really answer.
            return elapsed, False, "empty response"
        return elapsed, True, None

    except asyncio.TimeoutError:
        return time.perf_counter() - start, False, "timeout"  # Request took too long.
    except (ConnectionRefusedError, OSError) as e:
        return time.perf_counter() - start, False, type(e).__name__  # Connection failed.


async def run_rung(host, port, path, concurrency, total_requests, timeout, threshold_ms) -> dict:
    """
    Run one load-test step at one fixed concurrency level.
    Inputs:
        host, port, path: server target for each request
        concurrency: maximum requests running at the same time
        total_requests: total requests to send in this step
        timeout: maximum seconds each request may take
        threshold_ms: latency limit used to count slow successful requests
    What it does:
        Starts many request tasks, allows only `concurrency` of them to be
        active at once, waits for all of them to finish, then calculates
        throughput and latency stats.
    Async idea:
        `asyncio.gather` runs many coroutines together. The semaphore is the
        gate that keeps only N requests active at the same time.
    Output:
        A dictionary with success count, failure count, throughput, avg, p50,
        p95, p99, and max latency.
    """
    sem = asyncio.Semaphore(concurrency)  # Allow only this many requests at once.

    async def guarded():
        """Run one request after waiting for a concurrency slot.
        Inputs:
            Uses the server settings from run_rung().
        What it does:
            Enters the semaphore, sends one request, then automatically leaves
            the semaphore when the request finishes.
        Async idea:
            If all slots are busy, `async with sem` waits without blocking the
            whole event loop.
        Output:
            The tuple returned by one_request(). """
        async with sem:  # Wait here if the concurrency limit is already full.
            return await one_request(host, port, path, timeout)

    wall_start = time.perf_counter()  # Start timing this whole rung.
    results = await asyncio.gather( *(guarded() for _ in range(total_requests))
    )  # Run all request tasks.
    wall_elapsed = time.perf_counter() - wall_start  # Total time for this rung.

    latencies_ms = [r[0] * 1000.0 for r in results if r[1]]  # Successful request latencies in ms.
    errors = [r[2] for r in results if not r[1]]  # Error text from failed requests.
    slow_count = sum(1 for latency in latencies_ms if latency > threshold_ms)

    stats = {
        "concurrency": concurrency,  # How many requests were allowed at once.
        "sent": total_requests,  # Total request attempts in this rung.
        "ok": len(latencies_ms),  # Requests that got a non-empty response.
        "failed": len(errors),  # Requests that timed out or hit socket errors.
        "wall_s": wall_elapsed,  # Total seconds this rung took.
        "slow": slow_count,  # Successful requests slower than threshold_ms.
        "slow_pct": (slow_count / len(latencies_ms) * 100.0) if latencies_ms else 0.0,
        # Successful requests per second.
        "throughput": (len(latencies_ms) / wall_elapsed) if wall_elapsed > 0 else 0.0,
    }
    if latencies_ms:
        stats.update({
            "avg": statistics.mean(latencies_ms),
            "p50": percentile(latencies_ms, 50),
            "p95": percentile(latencies_ms, 95),
            "p99": percentile(latencies_ms, 99),
            "max": max(latencies_ms),
        })
    else:
        stats.update({"avg": None, "p50": None, "p95": None, "p99": None, "max": None})
    return stats


def percentile(latencies, p):
    """
    Calculate linear interpolation of the p-th percentile of a list of latencies.

    The latencies must be sorted first because percentiles are based on rank:
    p95 is the point where 95 percent of successful requests are at or below
    that latency, and about 5 percent are slower.
    """
    sorted_latencies = sorted(latencies)
    pos = (p / 100) * (len(sorted_latencies) - 1)
    low_idx = int(pos)
    high_idx = min(low_idx + 1, len(sorted_latencies) - 1)
    frac = pos - low_idx
    linear_inter = sorted_latencies[low_idx] + frac * (
        sorted_latencies[high_idx] - sorted_latencies[low_idx]
    )
    return linear_inter

def print_rung(s, threshold_ms):
    """
    Print the result of one tested concurrency level.

    Inputs:
        s: stats dictionary returned by run_rung()
        threshold_ms: p95 latency limit in milliseconds

    What it does:
        Prints success count, failure count, throughput, and latency numbers.
        It also marks the row if p95 crossed the threshold.

    Output:
        No return value; this function only prints.
    """
    if s["ok"] == 0:
        print(f"  conc={s['concurrency']:>5} | ALL {s['failed']} FAILED "
              f"| e.g. {_top_error(s)}")
        return

    tripped = s["p95"] is not None and s["p95"] > threshold_ms  # True when this rung is too slow.
    flag = "  <-- TRIPPED" if tripped else ""  # Add a visible marker for the bad row.
    print(
        f"  conc={s['concurrency']:>5} | "
        f"ok={s['ok']:>5} fail={s['failed']:>4} | "
        f"slow>{threshold_ms:.0f}ms={s['slow']:>4} ({s['slow_pct']:>5.1f}%) | "
        f"thrpt={s['throughput']:>8.1f} req/s | "
        f"avg={s['avg']:>7.1f} p50={s['p50']:>7.1f} "
        f"p95={s['p95']:>7.1f} p99={s['p99']:>7.1f} max={s['max']:>7.1f} ms"
        f"{flag}"
    )

def _top_error(s):
    """
    Return a short failure message for a failed rung.
    Inputs:
        s: stats dictionary returned by run_rung()
    What it does:
        Currently returns a simple generic message. Later this could count the
        exact error types, such as timeout or connection refused.
    Output:
        A short string for printing.
    """
    return "connection failures"


def save_csv(rows, host, port, path, threshold_ms, factor) -> str:
    """
    Write rung rows to a timestamped CSV file inside metrics/.
    Inputs:
        rows: list of dicts, each already tagged with "phase" and "note"
        host, port, path: server target (not written as columns)
        threshold_ms, factor: run settings, encoded into the filename
            instead of repeated as columns on every row
    Output:
        The path of the CSV file that was written.
    """
    os.makedirs("metrics", exist_ok=True)  # Create the folder if it doesn't exist yet.
    stamp = time.strftime("%Y%m%d_%H%M%S")  # e.g. 20260808_143200
    filename = f"metrics/run_{stamp}_slo{int(threshold_ms)}ms_factor{factor}.csv"
    fieldnames = ["phase", "concurrency", "ok", "failed", "throughput",
                  "avg", "p95", "max", "slow_pct", "note"]  # Only the columns we decided on.
    with open(filename, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            clean = {}
            for key in fieldnames:
                value = row.get(key, "")
                clean[key] = round(value, 1) if isinstance(value, float) else value  # Match the 1-decimal display used in the terminal.
            writer.writerow(clean)  # Drops any extra fields not in fieldnames, rounded values only.
    return filename


async def find_knee(low:int,hight:int,host,port,path,factor,timeout,threshold_ms,tolerance=10)-> dict:
    """
    we have last known concurency where SLO is achieved and firt concurency where SLO is not fulfilled
    the excet knee lies somewhere between these tow
    """
    lo, hi = low, hight  # lo = last confirmed good, hi = first confirmed bad
    steps = []  # every rung tested during bisection, tagged and kept for the CSV
    while hi - lo > tolerance:  # keep bisecting until the bracket is tight enough
        mid = (lo + hi) // 2  # next concurrency to test
        total = mid * factor  # same total-requests rule as the coarse ramp
        s = await run_rung(host, port, path, mid, total, timeout, threshold_ms)  # measure at mid
        print_rung(s, threshold_ms)  # show this bisection step live
        steps.append({**s, "phase": "bisect", "note": ""})  # tag this row and keep it
        good = s["p95"] is not None and s["p95"] <= threshold_ms  # oracle: did mid stay under the SLO?
        if good:
            lo = mid  # mid held up -> knee is above mid, raise the floor
        else:
            hi = mid  # mid broke the SLO -> knee is below mid, lower the ceiling
    return {"lo": lo, "hi": hi, "steps": steps}  # final narrowed bracket + every step tested

async def ramp(host, port, path, rungs, factor, threshold_ms, timeout, save_metrics=False):
    """
    Run the full load test by increasing concurrency step by step.
    Inputs:
        host, port, path: server target
        rungs: list of concurrency levels, for example [10, 25, 50, 100]
        factor: total requests per rung = concurrency * factor
        threshold_ms: stop when p95 latency goes above this number
        timeout: maximum seconds each request may take
        save_metrics: if True, write every rung tested to a CSV in metrics/
    What it does:
        Runs one rung at a time, prints each result, remembers the last good
        rung, and stops when latency or failures show the server is overloaded.
    Async idea:
        This function awaits each rung. Inside each rung, many requests run
        concurrently using asyncio.
    Output:
        A list of all rung result dictionaries collected during the test.
    """
    print(f"\nLoad testing http://{host}:{port}{path}")
    print(f"Trip rule: stop when p95 latency > {threshold_ms:.0f} ms\n")
    print("Ramping concurrency (each rung sends `factor x concurrency` requests):\n")

    last_good = None  # Last rung whose p95 stayed under the threshold.
    history = []  # Store every rung result.
    csv_rows = []  # Coarse + bisection rows, tagged, written out only if save_metrics is on.

    for c in rungs:
        total = c * factor  # Send several batches worth of requests.
        s = await run_rung( host, port, path, c, total, timeout, threshold_ms )  # Test this concurrency level.
        history.append(s)  # Save this rung's stats.
        csv_rows.append({**s, "phase": "coarse", "note": ""})  # Tag and keep this row for the CSV.
        print_rung(s, threshold_ms)  # Print this rung's stats.

        tripped_latency = s["p95"] is not None and s["p95"] > threshold_ms  # p95 is too slow.
        tripped_errors = s["ok"] == 0 or (s["failed"] > s["ok"])  # More failures than successes.

        if tripped_latency or tripped_errors:
            reason = "p95 latency crossed the threshold" if tripped_latency \
                     else "the server began failing the majority of requests"
            print(f"Tripped at concurrency = {c}  ({reason}).")
            print(" FINDING KNEE ....")
            csv_rows[-1]["note"] = "first_tripped"  # Mark the rung that triggered bisection.
            if last_good is not None:
                # knee is a point
                knee  = await find_knee(last_good['concurrency'],history[-1]['concurrency'],host,port,path,factor,timeout,threshold_ms)
                print(f"\nKnee bracket: {knee['lo']} (last good) -- {knee['hi']} (first bad)")
                csv_rows.extend(knee["steps"])  # Add every bisection step to the CSV rows.
                for row in csv_rows:  # Mark the final narrowed bracket edges.
                    if row["concurrency"] == knee["lo"]:
                        row["note"] = "final_knee_lower"
                    if row["concurrency"] == knee["hi"]:
                        row["note"] = "final_knee_upper"
            else:
                print("\nThe server tripped on the very FIRST rung -- lower the "
                      "starting concurrency or raise the threshold to find the knee.")
            if save_metrics:
                written = save_csv(csv_rows, host, port, path, threshold_ms, factor)
                print(f"\nSaved metrics to {written}")
            return history

        last_good = s  # This rung passed, so it becomes the latest safe result.
    print("\n" + "=" * 70)
    print("NO KNEE within the tested range")
    print("=" * 70)
    top = history[-1]  # Highest concurrency tested.
    print(f"Reached concurrency = {top['concurrency']} with p95 = {top['p95']:.1f} ms, "
          f"still under {threshold_ms:.0f} ms.")
    print("Raise the top of --rungs to push the server harder.\n")
    if save_metrics:
        written = save_csv(csv_rows, host, port, path, threshold_ms, factor)
        print(f"\nSaved metrics to {written}")
    return history


def parse_args():
    """
    Read command-line options for the load test.
    Inputs:
        The words typed after `python3 loadtest.py`.
    What it does:
        Defines options like --host, --port, --threshold-ms, --factor, and
        --rungs, then lets argparse parse the user's values.
    Output:
        An argparse Namespace object used by main().
    """
    ap = argparse.ArgumentParser(description="Ramping load test for the Raw HTTP server.")
    ap.add_argument("--host", default="127.0.0.1", help="server host (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=4221, help="server port (default 4221)")
    ap.add_argument("--path", default="/", help="request path (default '/', maps to index.html)")
    ap.add_argument("--threshold-ms", type=float, default=100.0,
                    help="p95 latency ceiling in ms; trip when exceeded (default 100)")
    ap.add_argument("--factor", type=int, default=10,
                    help="requests per rung = factor x concurrency (default 10)")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="per-request timeout in seconds (default 5)")
    ap.add_argument("--rungs", default="10,25,50,100,200,400,800,1600",
                    help="comma-separated concurrency levels to ramp through")
    ap.add_argument("--save-metrics", action="store_true",
                    help="save this run's rungs to a timestamped CSV in metrics/")
    return ap.parse_args()
    
    
def main():
    """
    Start the program.

    Inputs:
        Command-line arguments, read through parse_args().

    What it does:
        Converts --rungs from text into integers and starts the async ramp test.

    Async idea:
        `asyncio.run()` creates the event loop that drives all async requests.

    Output:
        No return value; results are printed to the terminal.
    """
    args = parse_args()  # Read command-line settings.
    try:
        rungs = [
            int(x) for x in args.rungs.split(",") if x.strip()
        ]  # Turn "10,25,50" into [10, 25, 50].
    except ValueError:
        print("--rungs must be comma-separated integers, e.g. 10,50,100,200", file=sys.stderr)
        sys.exit(2)

    try:
        asyncio.run(ramp(  # Create the event loop and run the async load test.
            host=args.host,
            port=args.port,
            path=args.path,
            rungs=rungs,
            factor=args.factor,
            threshold_ms=args.threshold_ms,
            timeout=args.timeout,
            save_metrics=args.save_metrics,
        ))
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        


if __name__ == "__main__":
    main()
