# test_metrics_http.py - /metrics and /metrics.json on a RUNNING server (Step 10).
# Needs the server (and its database) running. It does not read db.conf and writes nothing except normal page
# requests (a few 404s and a product page), so the counters move.
#     python tests\test_metrics_http.py
import http.client, json, re, socket, sys, time

HOST, PORT = "127.0.0.1", 8080
fails = 0

def get(path, host=HOST, timeout=10):
    c = http.client.HTTPConnection(host, PORT, timeout=timeout)
    c.request("GET", path)
    r = c.getresponse()
    body = r.read().decode("utf-8", "replace")
    out = (r.status, body, dict((k.lower(), v) for k, v in r.getheaders()))
    c.close()
    return out

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def metrics():
    s, b, h = get("/metrics.json")
    return json.loads(b) if s == 200 else None

s, b, h = get("/metrics")
check("GET /metrics from 127.0.0.1 -> 200 HTML", s == 200 and "text/html" in h.get("content-type", ""), str(s))
check("auto refresh every 2 s with a meta tag, no JavaScript",
      '<meta http-equiv="refresh" content="2">' in b and "<script" not in b)
for label in ("Uptime", "Total requests", "503 rejected", "408 timeouts", "Busy workers now", "Queue length now",
              "Free DB connections now", "Latency avg", "Latency p95", "Requests/sec", "Checkout COMMIT / ROLLBACK", "Dropped logs"):
    check("table row: " + label, label in b)

s, b, h = get("/metrics.json")
check("GET /metrics.json -> 200 application/json", s == 200 and "application/json" in h.get("content-type", ""), str(s))
m1 = json.loads(b)
for key in ("uptime_seconds", "total_requests", "status", "rejected_503", "timeouts_408", "busy_workers", "queue_length",
            "free_db_connections", "latency_ms", "requests_per_sec", "checkout", "dropped_logs"):
    check("json has '%s'" % key, key in m1)
check("busy_workers includes the /metrics request itself (>= 1) and is within the worker count",
      1 <= m1["busy_workers"] <= m1["workers"], str(m1["busy_workers"]))

# the counters move
before = metrics()
for p in ("/nope1", "/nope2", "/nope3"):
    get(p)                                           # three 404s
get("/products")
time.sleep(0.3)
after = metrics()
check("total_requests grew by at least 5 (3 x 404, /products, the first /metrics.json)",
      after["total_requests"] - before["total_requests"] >= 5, "%d -> %d" % (before["total_requests"], after["total_requests"]))
check("4xx grew by at least 3", after["status"]["4xx"] - before["status"]["4xx"] >= 3,
      "%d -> %d" % (before["status"]["4xx"], after["status"]["4xx"]))
check("2xx grew", after["status"]["2xx"] > before["status"]["2xx"])
check("latency numbers exist", after["latency_ms"]["samples"] > 0 and after["latency_ms"]["p95"] >= after["latency_ms"]["avg"] * 0.0)
check("requests_per_sec is positive right after traffic", after["requests_per_sec"] > 0, str(after["requests_per_sec"]))
check("uptime does not go backwards", after["uptime_seconds"] >= before["uptime_seconds"])

# query strings are not part of anything logged, and /metrics with a query still works
s, b, h = get("/metrics.json?x=1")
check("/metrics.json?x=1 works (path is matched without the query)", s == 200)

# only 127.0.0.1: the same server reached through this computer's LAN address must say 403
def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))             # no packet is sent; it only picks the outgoing interface
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return None

ip = lan_ip()
if ip and not ip.startswith("127."):
    s, b, h = get("/metrics", host=ip)
    check("GET /metrics via %s (not 127.0.0.1) -> 403" % ip, s == 403, str(s))
    s, b, h = get("/metrics.json", host=ip)
    check("GET /metrics.json via %s -> 403 and no numbers leaked" % ip, s == 403 and "total_requests" not in b, str(s))
else:
    print("SKIP the 403 check: this computer has no LAN address other than 127.0.0.1")

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
