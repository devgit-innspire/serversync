# test_static.py - Part 1 (static files): loading, content types, "../" tricks, headers and binary-exact bodies,
# using the real src/server.cpp code over a loopback socket. No database, no db.conf of yours: it builds its own
# temporary folder tree (with a decoy db.conf OUTSIDE the public folder) and deletes it afterwards.
#     python tests\test_static.py
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

work = tempfile.mkdtemp(prefix="serversync_static_")
try:
    root = os.path.join(work, "site")
    pub = os.path.join(root, "public")
    os.makedirs(os.path.join(pub, "sub"))
    def w(path, data):
        with open(os.path.join(pub, path), "wb") as f:
            f.write(data)
    w("style.css", b"body { color: #222; }\n")
    w("favicon.svg", b"<svg xmlns='http://www.w3.org/2000/svg'/>\n")
    w("logo.png", bytes(range(256)) * 8 + b"\x00\x00\xff\xfe" * 100)        # binary: NUL and 0xFF bytes
    w("app.js", b"console.log(1);\n")
    w("sub/deep.css", b"a{b:c}\n")
    w("UPPER.CSS", b"x{}\n")
    w("notes.txt", b"not a static type\n")
    w("archive.zip", b"PK\x03\x04")
    w("bad name.css", b"space in name\n")
    w("big.css", b"a" * (3 * 1024 * 1024))
    with open(os.path.join(root, "db.conf"), "w") as f:          # a DECOY next to (not inside) public/
        f.write("password=SECRET-DO-NOT-LEAK\n")

    exe = os.path.join(work, "static_check.exe")
    r = subprocess.run(["g++", "-std=c++20", "-Wall", "-o", exe, os.path.join(ROOT, "tests", "static_check.cpp"),
                        "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(BASE, "include"), "-lws2_32",
                        "-static-libgcc", "-static-libstdc++"],          # static runtime: the exe needs no DLLs
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:3000])
        sys.exit("build failed")
    p = subprocess.run([exe, pub], capture_output=True, text=True, cwd=root)
    out = p.stdout
    if p.returncode != 0 or "COUNT" not in out:
        print("harness did not run: exit code", p.returncode, p.stderr[:500])
        sys.exit(1)

    def find(name):
        m = re.search(r"^FIND \[%s\] (.*)$" % re.escape(name), out, re.M)
        return m.group(1) if m else None
    def http(name):
        m = re.search(r"^HTTP \[%s\] (.*?) \| body (\d+) sum (\d+)$" % re.escape(name), out, re.M)
        return (m.group(1), int(m.group(2)), int(m.group(3))) if m else None
    def hdr(name, key):
        m = re.search(r"^HDR \[%s\] %s: (.*)$" % (re.escape(name), key), out, re.M)
        return m.group(1).strip() if m else None
    def disk(name):
        m = re.search(r"^DISK \[%s\] (\d+) sum (\d+)$" % re.escape(name), out, re.M)
        return (int(m.group(1)), int(m.group(2))) if m else None

    # ---- loading ----
    check("loaded exactly the 6 files of a known type and safe name (css, svg, png, js, sub/deep.css, UPPER.CSS)",
          "COUNT 6" in out, re.search(r"COUNT \d+", out).group(0) if re.search(r"COUNT \d+", out) else out[:200])
    check("unknown types (.txt, .zip) are not served", find("notes.txt") == "null" and find("archive.zip") == "null")
    check("a name with a space is skipped (log says so)", find("bad name.css") == "null" and "skipped 'bad name.css'" in out)
    check("a file over 2 MB is skipped (not kept in memory)", find("big.css") == "null" and "skipped 'big.css'" in out)

    # ---- content types by extension ----
    check(".css -> text/css", (find("style.css") or "").startswith("found text/css; charset=utf-8"), find("style.css"))
    check(".svg -> image/svg+xml", (find("favicon.svg") or "").startswith("found image/svg+xml"))
    check(".png -> image/png", (find("logo.png") or "").startswith("found image/png"))
    check(".js -> application/javascript", (find("app.js") or "").startswith("found application/javascript"))
    check("a file in a sub folder is found as sub/deep.css", (find("sub/deep.css") or "").startswith("found text/css"))
    check("extension is case-insensitive (UPPER.CSS)", (find("UPPER.CSS") or "").startswith("found text/css"))

    # ---- path tricks: all must be 404 (null) ----
    for bad in ["../db.conf", "..\\db.conf", "sub/../../db.conf", "/etc/passwd", "/style.css", "..", "%2e%2e/db.conf",
                "sub\\deep.css", "", "a..b.css", "missing.css", "sub", "sub/"]:
        check("find_static(%r) -> nothing" % bad, find(bad) == "null", str(find(bad)))
    check("query string is not part of the name: 'style.css?x=1' is not a file", find("style.css?x=1") == "null")

    # ---- the exact HTTP answer over a real socket ----
    h = http("style.css")
    check("GET /static/style.css -> 200 OK", h and h[0] == "HTTP/1.1 200 OK", str(h))
    check("Content-Type: text/css; charset=utf-8", hdr("style.css", "Content-Type") == "text/css; charset=utf-8")
    check("Cache-Control: max-age=3600", hdr("style.css", "Cache-Control") == "max-age=3600")
    check("Content-Length matches the file", hdr("style.css", "Content-Length") == str(disk("style.css")[0]))
    check("css body is byte-identical to the file", h and (h[1], h[2]) == disk("style.css"))
    hp = http("logo.png")
    check("binary png (with NUL / 0xFF bytes) is byte-identical", hp and (hp[1], hp[2]) == disk("logo.png"), str(hp))
    check("png Content-Type: image/png", hdr("logo.png", "Content-Type") == "image/png")
    check("svg Content-Type: image/svg+xml", hdr("favicon.svg", "Content-Type") == "image/svg+xml")
    check("js Content-Type", hdr("app.js", "Content-Type") == "application/javascript; charset=utf-8")
    check("sub folder file is served", http("sub/deep.css")[0] == "HTTP/1.1 200 OK" and
          (http("sub/deep.css")[1], http("sub/deep.css")[2]) == disk("sub/deep.css"))

    for bad in ["../db.conf", "nope.css", "/x", ""]:
        hb = http(bad)
        check("GET /static/%s -> 404" % bad, hb and hb[0] == "HTTP/1.1 404 Not Found", str(hb))
    check("/static/../db.conf -> 404 and nothing of the secret file is sent", http("../db.conf")[1] < 40 and
          "SECRET" not in out)
    check("404 answers are not cacheable (no Cache-Control)", hdr("nope.css", "Cache-Control") is None)
    check("POST /static/style.css -> 405 with Allow: GET", "HTTP/1.1 405" in out and "POSTHDR Allow: GET" in out)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
