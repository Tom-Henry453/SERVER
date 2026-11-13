// loadtest.cpp - ramping load test for the Raw server.
//
// Same measurement discipline as loadtest_mp.py - held concurrency, equal-time
// rungs, median-of-N repeats, two-rung hysteresis, bisected knee, discarded
// warm-up, and the client_ms confession line - but the generator is C++23
// coroutines with one io_context per thread, so it stops being the bottleneck.
//
// build: g++ -std=c++23 -O2 -pthread loadtest.cpp -o loadtest
//        (macOS with Homebrew Boost: add -I/opt/homebrew/include)
// run:   ./loadtest --duration 2 --rungs 10,25,50,100,200,400,800,1600 --save-metrics

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <time.h>

namespace asio = boost::asio;
using tcp  = asio::ip::tcp;
using clk  = std::chrono::steady_clock;
using boost::system::error_code;
using namespace asio::experimental::awaitable_operators;

static double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}
static double thread_cpu_s() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ===========================================================================
// config
// ===========================================================================

struct Cfg {
    std::string   host = "127.0.0.1", path = "/";
    int           port = 4221;
    tcp::endpoint ep;
    std::string   request;                       // built once, sent verbatim
    std::chrono::milliseconds timeout{5000};
    bool          read_len = false;              // false = read to EOF
    int           workers = 8, warmup = 50, repeats = 3;
    double        duration = 2.0, threshold_ms = 100.0;
    std::vector<int> rungs;
    bool          save = false;
};

// EMFILE / EADDRNOTAVAIL are OUR fd table and OUR port table running dry, not
// the server refusing work. Counting them as server failures is how a load
// tester confidently reports a knee that is actually its own exhaustion.
static bool client_side(const error_code& ec) {
    switch (ec.value()) {
        case EMFILE: case ENFILE: case EADDRNOTAVAIL:
        case EADDRINUSE: case ENOBUFS: case ENOMEM: return true;
        default: return false;
    }
}

static size_t content_length(std::string_view hdr) {
    std::string low(hdr);
    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    auto p = low.find("content-length:");
    if (p == std::string::npos) return 0;
    return strtoull(low.c_str() + p + 15, nullptr, 10);
}

// ===========================================================================
// one request  (this is the whole async part)
// ===========================================================================

enum class Res { ok, client_err, server_err };

static asio::awaitable<Res> do_request(tcp::socket& s, const Cfg& c,
                                       std::string& buf, double& conn_ms) {
    auto tok = [](error_code& ec) { return asio::redirect_error(asio::use_awaitable, ec); };
    error_code ec;

    auto t0 = clk::now();
    co_await s.async_connect(c.ep, tok(ec));
    if (ec) co_return client_side(ec) ? Res::client_err : Res::server_err;
    conn_ms = ms_since(t0);

    s.set_option(tcp::no_delay(true), ec);

    co_await asio::async_write(s, asio::buffer(c.request), tok(ec));
    if (ec) co_return Res::server_err;

    buf.clear();
    if (c.read_len) {
        size_t hdr = co_await asio::async_read_until(
            s, asio::dynamic_buffer(buf), "\r\n\r\n", tok(ec));
        if (ec) co_return Res::server_err;
        size_t total = hdr + content_length(std::string_view(buf).substr(0, hdr));
        if (buf.size() < total)
            co_await asio::async_read(s, asio::dynamic_buffer(buf),
                                      asio::transfer_exactly(total - buf.size()), tok(ec));
        if (ec) co_return Res::server_err;
    } else {
        co_await asio::async_read(s, asio::dynamic_buffer(buf), tok(ec));
        if (ec && ec != asio::error::eof) co_return Res::server_err;
    }
    co_return buf.empty() ? Res::server_err : Res::ok;
}

struct Result {
    std::vector<double> lat_ms, conn_ms;
    long   client_err = 0, server_err = 0;
    double cpu_s = 0;
};

// One connection slot: request after request until the clock runs out. There
// is no semaphore and no request count - N slots means N in flight by
// construction, which is what "concurrency" actually means.
static asio::awaitable<void> slot(const Cfg& c, Result& r, clk::time_point deadline) {
    auto ex = co_await asio::this_coro::executor;
    std::string        buf;
    asio::steady_timer timer(ex);

    while (clk::now() < deadline) {
        tcp::socket s(ex);
        double      conn = 0;
        error_code  tec;
        timer.expires_after(c.timeout);

        auto t0 = clk::now();
        // Race the request against the timeout. `||` cancels the loser and
        // waits for it, so the socket outlives every operation touching it.
        auto v = co_await (do_request(s, c, buf, conn)
                        || timer.async_wait(asio::redirect_error(asio::use_awaitable, tec)));
        double el = ms_since(t0);

        Res res = v.index() == 0 ? std::get<0>(v) : Res::server_err;  // timer won = timeout
        if (res == Res::ok) {
            r.lat_ms.push_back(el);
            r.conn_ms.push_back(conn);
        } else {
            (res == Res::client_err ? r.client_err : r.server_err)++;
            // A dead server answers instantly; without this pause every slot
            // would spin flat-out logging junk.
            timer.expires_after(std::chrono::milliseconds(10));
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, tec));
        }
    }
}

// One thread, one io_context, its own slots. Connections never migrate between
// threads, so there is no strand, no lock, and no atomic on the hot path.
static Result run_worker(const Cfg& c, int share, clk::time_point deadline) {
    asio::io_context io(1);
    Result r;
    r.lat_ms.reserve(1 << 16);
    r.conn_ms.reserve(1 << 16);
    for (int i = 0; i < share; ++i)
        asio::co_spawn(io, slot(c, r, deadline), asio::detached);

    double cpu0 = thread_cpu_s();
    for (;;) {
        try { io.run(); break; }
        catch (const std::exception&) { r.server_err++; }
    }
    r.cpu_s = thread_cpu_s() - cpu0;
    return r;
}

// ===========================================================================
// stats
// ===========================================================================

static double pct(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return -1;
    double pos  = p / 100.0 * (sorted.size() - 1);
    size_t lo   = (size_t)pos, hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (pos - lo) * (sorted[hi] - sorted[lo]);
}

// 10 across 4 -> [3,3,2,2], which really is 10 (not 8).
static std::vector<int> split_evenly(int total, int parts) {
    std::vector<int> v;
    int base = total / parts, rem = total % parts;
    for (int i = 0; i < parts; ++i) {
        int s = base + (i < rem ? 1 : 0);
        if (s > 0) v.push_back(s);
    }
    return v;
}

struct Sample {
    int    concurrency = 0, ok = 0;
    long   client_err = 0, server_err = 0;
    double wall = 0, throughput = 0, avg = 0, p50 = 0, p95 = 0, p99 = 0,
           max = 0, conn_ms = 0, conn_max = 0, client_ms = 0;
    bool   overshoot = false;
    long   failed() const { return client_err + server_err; }
};

static Sample run_rung(const Cfg& c, int conc, double duration) {
    auto shares = split_evenly(conc, c.workers);
    std::vector<Result> out(shares.size());

    // Threads are created per rung (~20us each against a 2s rung). The Python
    // version needed a persistent pool only because forking costs milliseconds.
    auto wall0    = clk::now();
    auto deadline = wall0 + std::chrono::duration_cast<clk::duration>(
                                std::chrono::duration<double>(duration));
    {
        std::vector<std::jthread> th;
        for (size_t i = 0; i < shares.size(); ++i)
            th.emplace_back([&, i] { out[i] = run_worker(c, shares[i], deadline); });
    }
    double wall = std::chrono::duration<double>(clk::now() - wall0).count();

    Sample s;
    s.concurrency = std::accumulate(shares.begin(), shares.end(), 0);
    std::vector<double> lat, conn;
    for (auto& r : out) {
        lat.insert(lat.end(), r.lat_ms.begin(), r.lat_ms.end());
        conn.insert(conn.end(), r.conn_ms.begin(), r.conn_ms.end());
        s.client_err += r.client_err;
        s.server_err += r.server_err;
    }
    // THE CONFESSION. Worst case a finished reply waits behind all of its
    // thread's other in-flight replies before we read it:
    //     delay added by the client <= share x (CPU cost per reply)
    // Printed on every row instead of discovered three days later.
    for (size_t i = 0; i < shares.size(); ++i)
        if (!out[i].lat_ms.empty())
            s.client_ms = std::max(s.client_ms,
                shares[i] * (out[i].cpu_s / out[i].lat_ms.size()) * 1000.0);

    s.ok         = (int)lat.size();
    s.wall       = wall;
    s.throughput = wall > 0 ? lat.size() / wall : 0;
    // A slot stuck in a TCP retransmit runs past the deadline, so the rung ends
    // with most slots idle and throughput is divided by too long a wall.
    s.overshoot  = wall > duration * 1.25;
    if (!lat.empty()) {
        std::sort(lat.begin(), lat.end());
        std::sort(conn.begin(), conn.end());
        s.avg     = std::accumulate(lat.begin(), lat.end(), 0.0) / lat.size();
        s.p50     = pct(lat, 50);
        s.p95     = pct(lat, 95);
        s.p99     = pct(lat, 99);
        s.max     = lat.back();
        s.conn_ms = pct(conn, 50);
        // The connect TAIL, not the median: a ~1000/2000 ms conn_max is the
        // accept queue overflowing (SYN dropped -> 1s RTO), which is a totally
        // different fault from a slow response and must not look like one.
        s.conn_max = conn.back();
    }
    return s;
}

// One measurement is a lottery ticket. Sorting N samples by p95 and keeping the
// middle one throws away both the lucky run and the unlucky one. We keep a
// whole sample, never an average of fields.
static Sample measure_rung(const Cfg& c, int conc) {
    std::vector<Sample> v;
    for (int i = 0; i < c.repeats; ++i) v.push_back(run_rung(c, conc, c.duration));
    std::sort(v.begin(), v.end(), [](const Sample& a, const Sample& b) {
        return (a.ok ? a.p95 : 1e18) < (b.ok ? b.p95 : 1e18);
    });
    return v[v.size() / 2];
}

// p99 an order above p95 means a slice of requests failed a DIFFERENT way
// (SYN retransmit, timeout) rather than the whole server slowing down. p95 then
// sits above or below the SLO by coin flip, and a knee bisected on it is an
// artifact of the search, not a property of the server.
static bool bimodal(const Sample& s) { return s.ok && s.p99 > 5 * s.p95; }

static void print_rung(const Sample& s, double thr) {
    if (s.ok == 0) {
        printf("  conc=%5d | ALL %ld FAILED (client=%ld server=%ld)\n",
               s.concurrency, s.failed(), s.client_err, s.server_err);
        return;
    }
    char buf[192];
    std::string flag;
    if (s.p95 > thr) flag += "  <-- TRIPPED";
    if (s.client_ms > 0.25 * s.p95) {
        snprintf(buf, sizeof buf,
                 "  [WARNING: up to %.1f ms of this %.1f ms is the GENERATOR, "
                 "not the server]", s.client_ms, s.p95);
        flag += buf;
    }
    if (s.client_err > 0) {
        snprintf(buf, sizeof buf,
                 "  [%ld CLIENT-side errors (fd/port exhaustion) - not the server]",
                 s.client_err);
        flag += buf;
    }
    if (s.overshoot)
        flag += "  [rung overran its duration - a request stalled; throughput understated]";
    if (bimodal(s)) {
        snprintf(buf, sizeof buf,
                 "  [BIMODAL: p99 %.0f vs p95 %.0f - a slice failed a different "
                 "way, p95 is a coin flip here]", s.p99, s.p95);
        flag += buf;
    }
    printf("  conc=%5d | ok=%6d fail=%4ld | %4.1fs | thrpt=%8.1f req/s | "
           "avg=%6.1f p50=%6.1f p95=%6.1f p99=%6.1f max=%7.1f ms | "
           "conn=%4.1f/%-6.1f client<=%4.1fms%s\n",
           s.concurrency, s.ok, s.failed(), s.wall, s.throughput,
           s.avg, s.p50, s.p95, s.p99, s.max, s.conn_ms, s.conn_max,
           s.client_ms, flag.c_str());
}

static bool is_bad(const Sample& s, double thr) { return s.ok == 0 || s.p95 > thr; }

// ===========================================================================
// csv  (first 13 columns are loadtest_mp.py's, unchanged, so old runs still
//        plot against new ones; the rest are appended)
// ===========================================================================

struct Row { Sample s; std::string phase, note; };

static std::string save_csv(const std::vector<Row>& rows, const Cfg& c) {
    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&t));
    char name[256];
    snprintf(name, sizeof name, "metrics/run_%s_cpp_slo%dms_%gs.csv",
             stamp, (int)c.threshold_ms, c.duration);
    std::filesystem::create_directory("metrics");

    std::ofstream f(name);
    f << "phase,concurrency,workers,ok,failed,wall,throughput,avg,p50,p95,max,"
         "client_ms,note,client_err,server_err,conn_ms,conn_max,p99\n";
    f << std::fixed;
    f.precision(1);
    for (auto& [s, phase, note] : rows) {
        // A rung where nothing succeeded has no latency. Writing the 0 that
        // Sample was default-constructed with would plot as perfect latency.
        auto lat = [&](double v) {
            char b[32];
            if (!s.ok) return std::string();
            snprintf(b, sizeof b, "%.1f", v);
            return std::string(b);
        };
        f << phase << ',' << s.concurrency << ',' << c.workers << ',' << s.ok << ','
          << s.failed() << ',' << s.wall << ',' << s.throughput << ',' << lat(s.avg) << ','
          << lat(s.p50) << ',' << lat(s.p95) << ',' << lat(s.max) << ',' << s.client_ms << ','
          << note << ',' << s.client_err << ',' << s.server_err << ','
          << lat(s.conn_ms) << ',' << lat(s.conn_max) << ',' << lat(s.p99) << '\n';
    }
    return name;
}

// ===========================================================================
// the ramp
// ===========================================================================

static void ramp(const Cfg& c) {
    printf("\nLoad testing http://%s:%d%s  [read-mode: %s]\n",
           c.host.c_str(), c.port, c.path.c_str(), c.read_len ? "length" : "eof");
    printf("Generator : %d threads, one io_context each (%u cores)\n",
           c.workers, std::thread::hardware_concurrency());
    printf("Each rung : %gs of load, measured %dx, median kept\n", c.duration, c.repeats);
    printf("Trip rule : TWO rungs in a row with p95 > %.0f ms\n\n", c.threshold_ms);

    // Warm-up, thrown away on purpose: index.html into the page cache, CPU at
    // full clock, allocators warm, before the first real rung.
    printf("  [warm-up rung: conc=%d for 1s, result discarded]\n", c.warmup);
    run_rung(c, c.warmup, 1.0);

    std::vector<Row> rows;
    Sample      suspect, last_good;
    bool        have_suspect = false, have_good = false, stopped = false;
    std::string verdict = "no_knee";

    for (int conc : c.rungs) {
        Sample s = measure_rung(c, conc);
        print_rung(s, c.threshold_ms);

        if (s.ok == 0) {
            rows.push_back({s, "coarse", "server_down"});
            printf("\nServer failed EVERY request at concurrency = %d.\n", s.concurrency);
            verdict = "server_down";
            stopped = true;
            break;
        }
        if (s.p95 > c.threshold_ms) {
            // A rung that breached ONCE is a suspect, not a verdict.
            if (!have_suspect) {
                suspect = s; have_suspect = true;
                rows.push_back({s, "coarse", "breach_candidate"});
                printf("    [breach at conc=%d - not trusted yet, the next rung decides]\n",
                       s.concurrency);
                continue;
            }
            rows.push_back({s, "coarse", "breach_confirmed"});
            printf("\nCONFIRMED: two rungs in a row over %.0f ms (conc=%d then %d).\n",
                   c.threshold_ms, suspect.concurrency, s.concurrency);
            if (bimodal(s) || bimodal(suspect)) {
                // Bisecting a coin flip returns a number, and the number is the
                // search's, not the server's. Say so instead of printing it.
                printf("\nNOT bisecting: the tail is bimodal (p99 >> p95), so p95\n"
                       "crosses the SLO by luck. Check conn_max first - ~1000/2000 ms\n"
                       "means the accept queue is overflowing, not the handler.\n");
                verdict = "bimodal";
            } else if (c.repeats < 3) {
                printf("\nNOT bisecting: --repeats %d. The knee search trusts one\n"
                       "sample per midpoint, which is only safe when each is a\n"
                       "median-of-N. Re-run with --repeats 3.\n", c.repeats);
                verdict = "unconfirmed";
            } else if (have_good) {
                printf(" FINDING KNEE ...\n");
                // Safe to trust one answer per midpoint: each is a median-of-3.
                int lo = last_good.concurrency, hi = suspect.concurrency;
                while (hi - lo > 10) {
                    int    mid = (lo + hi) / 2;
                    Sample m   = measure_rung(c, mid);
                    print_rung(m, c.threshold_ms);
                    rows.push_back({m, "bisect", ""});
                    (is_bad(m, c.threshold_ms) ? hi : lo) = mid;
                }
                printf("\nKnee bracket: %d (last good) -- %d (first bad)\n", lo, hi);
                verdict = "knee_found";
            } else {
                printf("\nTripped on the FIRST rung - lower --rungs.\n");
                verdict = "tripped_first_rung";
            }
            stopped = true;
            break;
        }
        if (have_suspect) {
            printf("    [conc=%d breach NOT confirmed - it was noise, forgetting it]\n",
                   suspect.concurrency);
            rows.push_back({s, "coarse", "false_alarm_cleared"});
            have_suspect = false;
        } else {
            rows.push_back({s, "coarse", ""});
        }
        last_good = s; have_good = true;
    }
    if (!stopped) {
        if (have_suspect) {
            verdict = "unconfirmed";
            printf("\nRamp ended on an UNCONFIRMED breach at conc=%d - add a higher rung.\n",
                   suspect.concurrency);
        } else if (have_good) {
            printf("\nNo trip within the tested range (top rung %d: p95=%.1f ms) - raise --rungs.\n",
                   last_good.concurrency, last_good.p95);
        }
    }
    if (c.save && !rows.empty())
        printf("Saved metrics to %s  [%s]\n", save_csv(rows, c).c_str(), verdict.c_str());
}

// ===========================================================================
// main
// ===========================================================================

static std::string arg(int argc, char** argv, const char* n, std::string def) {
    for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], n)) return argv[i + 1];
    return def;
}
static bool has(int argc, char** argv, const char* n) {
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], n)) return true;
    return false;
}

static void self_test() {
    std::vector<double> v{1, 2, 3, 4, 5};
    assert(pct(v, 50) == 3);
    assert(pct(v, 0) == 1 && pct(v, 100) == 5);
    assert(pct(v, 95) == 4.8);
    assert((split_evenly(10, 4) == std::vector<int>{3, 3, 2, 2}));
    assert((split_evenly(2, 4) == std::vector<int>{1, 1}));       // no zero shares
    assert(content_length("HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n") == 42);
    assert(content_length("hdr\r\ncontent-length:7\r\n\r\n") == 7);
    assert(content_length("HTTP/1.1 200 OK\r\n\r\n") == 0);
    puts("self-test ok");
}

int main(int argc, char** argv) {
    if (has(argc, argv, "--self-test")) { self_test(); return 0; }

    Cfg c;
    c.host         = arg(argc, argv, "--host", c.host);
    c.port         = atoi(arg(argc, argv, "--port", "4221").c_str());
    c.path         = arg(argc, argv, "--path", c.path);
    c.threshold_ms = atof(arg(argc, argv, "--threshold-ms", "100").c_str());
    c.duration     = atof(arg(argc, argv, "--duration", "2").c_str());
    c.timeout      = std::chrono::milliseconds(
                         (long)(1000 * atof(arg(argc, argv, "--timeout", "5").c_str())));
    c.warmup       = atoi(arg(argc, argv, "--warmup", "50").c_str());
    c.repeats      = atoi(arg(argc, argv, "--repeats", "3").c_str());
    c.read_len     = arg(argc, argv, "--read-mode", "eof") == "length";
    c.save         = has(argc, argv, "--save-metrics");

    int cores   = (int)std::thread::hardware_concurrency();
    int reserve = atoi(arg(argc, argv, "--reserve", "4").c_str());
    c.workers   = atoi(arg(argc, argv, "--workers",
                          std::to_string(std::max(2, cores - reserve))).c_str());

    std::string r = arg(argc, argv, "--rungs", "10,25,50,100,200,400,800,1600");
    for (size_t p = 0; p <= r.size(); ) {
        size_t q = r.find(',', p);
        if (q == std::string::npos) q = r.size();
        if (q > p) c.rungs.push_back(atoi(r.substr(p, q - p).c_str()));
        p = q + 1;
    }
    if (c.rungs.empty()) { fputs("--rungs must be comma-separated integers\n", stderr); return 1; }

    c.request = "GET " + c.path + " HTTP/1.1\r\nHost: " + c.host +
                "\r\nConnection: close\r\n\r\n";
    try {
        asio::io_context io;
        c.ep = *tcp::resolver(io).resolve(c.host, std::to_string(c.port)).begin();
    } catch (const std::exception& e) {
        fprintf(stderr, "cannot resolve %s: %s\n", c.host.c_str(), e.what());
        return 1;
    }

    // Raise our own fd ceiling before blaming the server for EMFILE.
    rlimit rl{};
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    int top = *std::max_element(c.rungs.begin(), c.rungs.end());
    if ((rlim_t)top * 2 > rl.rlim_cur)
        printf("  [WARNING: top rung %d vs fd limit %lu - raise it "
               "(`ulimit -n 65535`) or rungs will fail on OUR side]\n",
               top, (unsigned long)rl.rlim_cur);
    // Every connection needs a free client port. TIME_WAIT lands on whichever
    // side closes first - today that is the server (`Connection: close`), so
    // our ports recycle fast. If that ever flips, the ceiling below becomes a
    // per-60s budget instead of a concurrency limit, and client_err will say so.
    if (std::ifstream pr("/proc/sys/net/ipv4/ip_local_port_range"); pr) {
        long lo, hi; pr >> lo >> hi;
        printf("  [client ports %ld-%ld = %ld usable; widen with sysctl "
               "net.ipv4.ip_local_port_range if client_err appears]\n", lo, hi, hi - lo);
    }

    ramp(c);
    return 0;
}
