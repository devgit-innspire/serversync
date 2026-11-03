# test_static_http.py - /static/... on a RUNNING server (Part 1 + the new look). Needs the server (and its database)
# running from the project folder. It only reads files; it does not read db.conf and writes nothing.
#     python tests\test_static_http.py
import http.client, re, socket, sys

HOST, PORT = "127.0.0.1", 8080
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def raw(request_line):
    """send a request line exactly as written (a normal HTTP client would clean up ../ before sending)"""
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.sendall((request_line + "\r\nHost: localhost\r\nConnection: close\r\n\r\n").encode("latin-1"))
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    return lines[0], headers, body

status, h, body = raw("GET /static/style.css HTTP/1.1")
check("GET /static/style.css -> 200", status == "HTTP/1.1 200 OK", status)
check("Content-Type text/css", h.get("content-type", "").startswith("text/css"), h.get("content-type", ""))
check("Cache-Control: max-age=3600", h.get("cache-control") == "max-age=3600", str(h.get("cache-control")))
check("Content-Length matches the body", h.get("content-length") == str(len(body)))
check("it is the real stylesheet (variables + dark mode)", b"--primary" in body and b"prefers-color-scheme: dark" in body)

status, h, body = raw("GET /static/app.js HTTP/1.1")
check("GET /static/app.js -> 200 application/javascript (the popups' JavaScript)",
      status == "HTTP/1.1 200 OK" and h.get("content-type", "").startswith("application/javascript") and b"openModal" in body, status)

status, h, body = raw("GET /static/favicon.svg HTTP/1.1")
check("GET /static/favicon.svg -> 200 image/svg+xml", status == "HTTP/1.1 200 OK" and h.get("content-type") == "image/svg+xml", status)

for line, why in [("GET /static/../db.conf HTTP/1.1", "../ trick"), ("GET /static/..\\db.conf HTTP/1.1", "backslash"),
                  ("GET /static//db.conf HTTP/1.1", "leading slash"), ("GET /static/%2e%2e/db.conf HTTP/1.1", "encoded .."),
                  ("GET /static/sub/../../db.conf HTTP/1.1", "deeper ../"), ("GET /static/nope.css HTTP/1.1", "unknown file"),
                  ("GET /static/ HTTP/1.1", "empty name")]:
    status, h, body = raw(line)
    check("%s -> 404" % why, status == "HTTP/1.1 404 Not Found", status)
    check("   ...and no password text in the answer", b"password" not in body.lower() and b"host=" not in body.lower())

status, h, body = raw("POST /static/style.css HTTP/1.1")
check("POST /static/style.css -> 405", status.startswith("HTTP/1.1 405"), status)

# the pages use the stylesheet, not inline CSS
status, h, body = raw("GET / HTTP/1.1")
page = body.decode("utf-8", "replace")
check("home page: <link rel=stylesheet href=/static/style.css>, favicon, no inline <style>",
      '<link rel="stylesheet" href="/static/style.css">' in page and "/static/favicon.svg" in page and "<style" not in page)
check("home page (logged out): hero 'Start shopping', login + signup popups that POST to the same routes, app.js",
      "Start shopping" in page and 'id="loginModal"' in page and 'id="signupModal"' in page and 'action="/login"' in page and
      'action="/signup"' in page and '<script src="/static/app.js" defer></script>' in page)
check("home page: category tiles link to /products?cat=N and there is a 'Budget picks' grid",
      "/products?cat=" in page and "Budget picks under" in page and "₹" in page)
check("home page comes from layout.html (viewport meta, navbar, main, footer)",
      'name="viewport"' in page and 'class="topbar"' in page and '<main class="container">' in page and "site-footer" in page)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
