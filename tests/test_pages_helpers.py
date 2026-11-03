# test_pages_helpers.py - builds and runs tests/pages_helpers_check.cpp (no database, no server, no db.conf):
# the pure helpers in pages.cpp, the /metrics pages (HTML + JSON + the 127.0.0.1-only rule) and the sold-out handling.
#     python tests\test_pages_helpers.py
import json, os, subprocess, sys, tempfile
sys.stdout.reconfigure(encoding="utf-8", errors="replace")      # output contains the rupee sign

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
tmp = tempfile.mkdtemp(prefix="serversync_helpers_")
exe = os.path.join(tmp, "pages_helpers_check.exe")
code = 1
try:
    c = subprocess.run(["g++", "-std=c++20", "-Wall", "-o", exe, os.path.join(ROOT, "tests", "pages_helpers_check.cpp"),
                        "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(BASE, "include"),
                        "-lws2_32", "-lbcrypt", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
    if c.returncode != 0:
        print(c.stderr[:3000])
        sys.exit("build failed")
    r = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", cwd=ROOT)      # cwd = project folder: it loads templates/
    lines = r.stdout.splitlines()
    for l in lines:
        if not l.startswith("JSON "):
            print(l)
    extra_fails = 0
    js = [l[5:] for l in lines if l.startswith("JSON ")]
    try:
        d = json.loads(js[0])
        ok = (d["uptime_seconds"] == 3725 and d["total_requests"] == 120 and d["status"] == {"2xx": 90, "3xx": 20, "4xx": 7, "5xx": 3}
              and d["rejected_503"] == 2 and d["timeouts_408"] == 1 and d["busy_workers"] == 2 and d["queue_length"] == 5
              and d["free_db_connections"] == 3 and d["latency_ms"] == {"avg": 12.35, "p95": 80.5, "samples": 100}
              and d["requests_per_sec"] == 4.25 and d["checkout"] == {"commit": 6, "rollback": 1} and d["dropped_logs"] == 9)
        print(("PASS" if ok else "FAIL") + " metrics.json is valid JSON with all the fields and right values | keys: " + ", ".join(d.keys()))
        extra_fails += 0 if ok else 1
    except Exception as e:
        print("FAIL metrics.json is not valid JSON:", e, js[:1])
        extra_fails += 1
    code = 1 if (r.returncode != 0 or extra_fails or "FAIL " in r.stdout.replace("FAILURES", "")) else 0
finally:
    for f in os.listdir(tmp):               # only the temp folder created above
        os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
sys.exit(code)
