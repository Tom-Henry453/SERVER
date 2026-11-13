#!/bin/bash
# Closed-loop baseline for the Raw server, driven by Fortio.
#
#   usage: metrics/mac_result/run_baseline.sh        (run from the repo root)
#
# Builds the server from the current checkout, records the machine state,
# then runs a ladder of connection counts, three 10-second runs per level.
# Keep-alive is off because the server closes every connection.
# Results: metrics/mac_result/baseline_<commit>/{json/,environment.txt,progress.log}
#
# Before running: plug in, turn Low Power Mode off, close heavy apps, and raise
# the accept queue limit (resets on reboot):  sudo sysctl -w kern.ipc.somaxconn=2048
set -u
LEVELS="1 4 10 25 50 100 200 400 800"
RUNS=3
DURATION=10s
URL=http://127.0.0.1:4221/
COMMIT=$(git rev-parse --short HEAD)
OUT=metrics/mac_result/baseline_$COMMIT
mkdir -p "$OUT/json"
LOG=$OUT/progress.log
: > "$LOG"

if ! git diff --quiet HEAD -- '*.cpp' '*.hh'; then
    echo "source files differ from commit $COMMIT; commit first" | tee -a "$LOG"; exit 1
fi
g++ -std=c++23 -O2 -pthread -I. SERVER.cpp -o server_fixed || { echo "build failed" | tee -a "$LOG"; exit 1; }

# wait (up to 3 minutes) for the machine to go quiet
for i in $(seq 1 36); do
    L=$(sysctl -n vm.loadavg | awk '{print $2}')
    awk -v l="$L" 'BEGIN{exit !(l < 3.0)}' && break
    sleep 5
done

{
    echo "commit=$COMMIT"
    echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "cpu=$(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cores"
    echo "os=macOS $(sw_vers -productVersion)"
    echo "compiler=$(g++ --version | head -1)"
    echo "build=g++ -std=c++23 -O2 -pthread -I. SERVER.cpp"
    echo "tool=fortio $(fortio version)"
    echo "kern.ipc.somaxconn=$(sysctl -n kern.ipc.somaxconn)"
    echo "power=$(pmset -g batt | head -1)"
    echo "lowpowermode=$(pmset -g | awk '/lowpowermode/{print $2}')"
    echo "load_before=$(sysctl -n vm.loadavg)"
    echo "levels=$LEVELS runs=$RUNS duration=$DURATION keepalive=off target=$URL"
} > "$OUT/environment.txt"

pkill -x server_fixed; sleep 1
rm -f Logs.txt
./server_fixed > /dev/null 2>&1 &
SERVER_PID=$!
sleep 1.5
if ! curl -s -o /dev/null -m 5 "$URL"; then echo "server did not answer" | tee -a "$LOG"; exit 1; fi

# warm-up, discarded
fortio load -qps 0 -c 50 -t 3s -keepalive=false -loglevel Critical "$URL" > /dev/null 2>&1

for C in $LEVELS; do
    for R in $(seq 1 $RUNS); do
        F=$OUT/json/closed_c$(printf %03d "$C")_run$R.json
        fortio load -qps 0 -c "$C" -t "$DURATION" -keepalive=false -r 0.0001 \
            -p 50,75,90,95,99,99.9 -loglevel Critical \
            -labels "baseline closed c=$C run=$R commit=$COMMIT" -json "$F" "$URL" 2>&1 |
            awk -v c="$C" -v r="$R" '/^Code/{codes=codes $2"="$4" "}
                 /^All done/{printf "c=%-4d run%d  %9.1f req/s  avg %7.3f ms  %s\n",c,r,$(NF-1),$(NF-4),codes}' >> "$LOG"
        if ! kill -0 $SERVER_PID 2>/dev/null; then echo "SERVER DIED during c=$C run $R" >> "$LOG"; exit 2; fi
    done
done

echo "load_after=$(sysctl -n vm.loadavg)" >> "$OUT/environment.txt"
kill $SERVER_PID
echo "DONE" >> "$LOG"
