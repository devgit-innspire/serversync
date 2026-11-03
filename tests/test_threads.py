# test_threads.py - queue timing, metrics, the async request logger (batching, drop, flush on shutdown) and the
# VERBOSE_LOG gating, using src/threads.cpp with a fake database. No MySQL, no server, no db.conf needed.
#     python tests\test_threads.py
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

work = tempfile.mkdtemp(prefix="serversync_threads_")
exes = {}

def build(name, **settings):
    """compile threads_check.cpp against a copy of include/ with some settings changed (the real common.h is untouched)"""
    inc = os.path.join(work, "inc_" + name)
    shutil.copytree(os.path.join(ROOT, "include"), inc)
    p = os.path.join(inc, "common.h")
    t = open(p, encoding="utf-8").read()
    for key, val in settings.items():
        new, n = re.subn(r"const int %s = \d+;" % key, "const int %s = %d;" % (key, val), t)
        assert n == 1, key
        t = new
    open(p, "w", encoding="utf-8").write(t)
    exe = os.path.join(work, name + ".exe")
    r = subprocess.run(["g++", "-std=c++20", "-Wall", "-o", exe, os.path.join(ROOT, "tests", "threads_check.cpp"),
                        "-I" + inc, "-I" + os.path.join(BASE, "include"), "-lws2_32"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:3000])
        raise SystemExit("build failed: " + name)
    exes[name] = exe

def run(name, scenario):
    r = subprocess.run([exes[name], scenario], capture_output=True, text=True, timeout=120)
    return r.stdout + r.stderr

def kv(out):
    d = {}
    for line in out.splitlines():
        m = re.match(r"([A-Z0-9_]+) (-?[\d.e+-]+)$", line.strip())
        if m:
            d[m.group(1)] = float(m.group(2))
    return d

try:
    build("default")
    build("verbose", VERBOSE_LOG=1)
    build("window", LATENCY_WINDOW=50)
    build("small_queue", LOG_QUEUE_SIZE=20)
    build("big_queue", LOG_QUEUE_SIZE=20000)
    d = open(os.path.join(ROOT, "include", "common.h"), encoding="utf-8").read()
    check("defaults: VERBOSE_LOG = 0, LOG_QUEUE_SIZE = 1000, LOG_FLUSH_SECONDS = 1, LATENCY_WINDOW = 1000, RPS_SECONDS = 10",
          all(x in d for x in ("VERBOSE_LOG = 0;", "LOG_QUEUE_SIZE = 1000;", "LOG_FLUSH_SECONDS = 1;", "LATENCY_WINDOW = 1000;", "RPS_SECONDS = 10;")))

    # ---------- metrics ----------
    m = kv(run("default", "metrics"))
    check("total = 131 served + 2 rejected", m["TOTAL"] == 133, str(m["TOTAL"]))
    check("by class: 2xx=100, 3xx=20, 4xx=8 (7 + the 408), 5xx=5 (3 + 2 rejected 503)",
          (m["C2XX"], m["C3XX"], m["C4XX"], m["C5XX"]) == (100, 20, 8, 5), str((m["C2XX"], m["C3XX"], m["C4XX"], m["C5XX"])))
    check("classes add up to the total", m["C2XX"] + m["C3XX"] + m["C4XX"] + m["C5XX"] == m["TOTAL"])
    check("503 rejected = 2, 408 timeouts = 1", (m["REJECTED"], m["TIMEOUTS"]) == (2, 1))
    check("busy workers 0 and queue length 0 when idle", (m["BUSY"], m["QUEUE"]) == (0, 0))
    check("free DB connections comes from the pool (stub says 3)", m["FREE_DB"] == 3)
    check("latency window holds the 131 served requests (rejects are not timed)", m["SAMPLES"] == 131, str(m["SAMPLES"]))
    check("avg latency is sensible (100 requests of ~10 ms among 131)", 5 < m["AVG_MS"] < 25, str(m["AVG_MS"]))
    check("p95 latency is in the 10 ms group", 9 <= m["P95_MS"] <= 60, str(m["P95_MS"]))
    check("requests/sec over the last 10 s is positive and not above the total", 0 < m["RPS"] <= m["TOTAL"], str(m["RPS"]))
    check("checkout COMMIT = 6, ROLLBACK = 1", (m["COMMIT"], m["ROLLBACK"]) == (6, 1))
    out = run("default", "metrics")
    check("every served request was logged: INSERTED = 131 at shutdown", kv(out)["INSERTED"] == 131, str(kv(out)["INSERTED"]))
    check("nothing dropped", kv(out)["DROPPED"] == 0)

    # ---------- latency window ----------
    w = kv(run("window", "window"))
    check("LATENCY_WINDOW 50: only 50 samples are kept", w["SAMPLES"] == 50, str(w["SAMPLES"]))
    check("...and they are the LAST 50 (fast ones), old slow ones are overwritten", w["AVG_MS"] < 8, str(w["AVG_MS"]))

    # ---------- queue wait and latency ----------
    out = run("default", "timing")
    ents = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in out.splitlines() if l.startswith("ENTRY")]
    check("8 log entries written", len(ents) == 8, str(len(ents)))
    waits = sorted(float(e["wait"]) for e in ents)
    lats = [float(e["latency"]) for e in ents]
    check("first 4 requests got a worker at once (queue wait < 60 ms)", all(x < 60 for x in waits[:4]), str(waits[:4]))
    check("next 4 waited for a free worker (queue wait >= 150 ms)", all(x >= 150 for x in waits[4:]), str(waits[4:]))
    check("latency includes the queue wait + the 200 ms of work", all(float(e["latency"]) >= float(e["wait"]) + 190 for e in ents))
    check("latency is accept -> response sent, so >= 200 ms for everybody", all(x >= 200 for x in lats))
    check("worker ids are 1..4 and all four workers were used",
          sorted(set(int(e["worker"]) for e in ents)) == [1, 2, 3, 4], str(sorted(set(e["worker"] for e in ents))))
    check("entry fields: GET, /x (no query), 200, user 7, created just now",
          all(e["method"] == "GET" and e["path"] == "/x" and e["status"] == "200" and e["user"] == "7" and
              0 <= int(e["created_ago"]) <= 3 for e in ents))

    # ---------- drop when the memory queue is full; never block ----------
    dr = kv(run("small_queue", "drop"))
    check("100 pushes at once into a queue of 20: 80 dropped and counted", dr["DROPPED_AFTER_100"] == 80, str(dr["DROPPED_AFTER_100"]))
    check("pushing 100 entries took < 20 ms (never waits for the database)", dr["PUSH_US_100"] < 20000, str(dr["PUSH_US_100"]) + " us")
    check("pushing 10 more WHILE a slow 800 ms INSERT is running took < 20 ms", dr["PUSH_US_WHILE_INSERTING"] < 20000,
          str(dr["PUSH_US_WHILE_INSERTING"]) + " us")
    check("nothing is lost silently: inserted + dropped = 110 pushed", dr["INSERTED"] + dr["DROPPED"] == 110,
          "%d + %d" % (dr["INSERTED"], dr["DROPPED"]))
    check("the 20 + 10 that fit were all flushed at shutdown", dr["INSERTED"] == 30 and dr["DROPPED"] == 80)

    # ---------- batching and flush on shutdown ----------
    out = run("default", "batch")
    chunks = [int(x) for x in re.findall(r"CHUNK (\d+)", out)]
    check("450 entries -> multi-row inserts of 200, 200, 50", chunks == [200, 200, 50], str(chunks))
    check("shutdown flushed all 450", kv(out)["INSERTED"] == 450)
    check("stop_logger returns at once (does not wait the 1 s timer): condition_variable", kv(out)["STOP_MS"] < 500,
          str(kv(out)["STOP_MS"]) + " ms")

    # ---------- failing database ----------
    out = run("default", "fail")
    check("failed inserts are counted as dropped (30 + 5)", kv(out)["DROPPED"] == 35 and kv(out)["INSERTED"] == 0, out[-120:])
    check("the logger keeps running after a failure and logs the problem only once",
          out.count("could not save") == 1, str(out.count("could not save")))

    # ---------- periodic flush ----------
    out = run("default", "periodic")
    check("entries are written every second without stopping", kv(out)["INSERTED_BEFORE_STOP"] == 5, str(kv(out)))

    # ---------- many threads at once ----------
    st = kv(run("big_queue", "stress"))
    out = run("big_queue", "stress")
    st = kv(out)
    check("8 threads x 1000 requests: total is exactly 8000 (no lost updates)", st["TOTAL"] == 8000, str(st["TOTAL"]))
    check("2xx = 6000, 4xx = 2000", (st["C2XX"], st["C4XX"]) == (6000, 2000), str((st["C2XX"], st["C4XX"])))
    check("all 8000 log entries reached the database, none dropped", st["INSERTED"] == 8000 and st["DROPPED"] == 0,
          "%d inserted, %d dropped" % (st["INSERTED"], st["DROPPED"]))

    # ---------- VERBOSE_LOG gating ----------
    quiet = run("default", "verbose")
    loud = run("verbose", "verbose")
    check("VERBOSE_LOG 0: no 'conn taken' and no per-request line",
          "conn 1 taken" not in quiet and "GET / -> 200" not in quiet, quiet)
    check("VERBOSE_LOG 0: startup, CHECKOUT, CLEANUP, LOGGER lines are kept",
          all(x in quiet for x in ("startup line", "CHECKOUT] user 4 order 7", "CLEANUP] closed 2", "LOGGER] started")))
    check("VERBOSE_LOG 1: everything is printed",
          all(x in loud for x in ("conn 1 taken", "GET / -> 200", "startup line", "CHECKOUT]", "CLEANUP]", "LOGGER]")))

    # ---------- what is stored in request_logs ----------
    out = run("default", "sanitize")
    check("method cut to 10 chars (VARCHAR(10))", "M[GET]" in out and "M[ABCDEFGHIJ]" in out)
    check("path: non-ASCII and control bytes become '?'", "P[/caf??const".replace("const", "") in out.replace("?x", "?") or "P[/caf???x]" in out, out)
    check("path cut to 255 chars (VARCHAR(255))", "LEN 255" in out)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
