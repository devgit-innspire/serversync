# test_auth.py - end-to-end test for signup / login / session / logout (Step 7)
#
# NOTE: this test CREATES a user (test_<timestamp>@example.com) in the database that the running
# server is connected to. Only run it against a database where that is OK.
# It never reads or writes db.conf: start the server yourself, then run:
#     python tests\test_auth.py
import http.client, re, sys, time

HOST, PORT = "127.0.0.1", 8080
fails = 0

def req(method, path, form=None, cookie=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    headers = {}
    body = None
    if form is not None:
        from urllib.parse import urlencode
        body = urlencode(form)
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    if cookie:
        headers["Cookie"] = cookie
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    text = r.read().decode("utf-8")
    result = (r.status, text, r.getheaders())
    c.close()
    return result

def header(headers, name):
    return [v for k, v in headers if k.lower() == name.lower()]

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

email = "test_%d@example.com" % int(time.time())
good = {"name": "Test User", "email": email, "password": "secret1"}

# --- signup ---
s, b, h = req("GET", "/signup");                                  check("GET /signup form", s == 200 and "<form" in b)
s, b, h = req("POST", "/signup", {**good, "name": ""});          check("empty name -> 400", s == 400 and "fill in all" in b)
s, b, h = req("POST", "/signup", {**good, "email": "nope"});     check("email without @ -> 400", s == 400)
s, b, h = req("POST", "/signup", {**good, "password": "12345"}); check("short password -> 400", s == 400)
s, b, h = req("POST", "/signup", {**good, "name": "<script>x</script>"})
check("name is escaped when form is shown again", "<script>x" not in b)
s, b, h = req("POST", "/signup", good)
check("valid signup -> 302 /login?registered=1", s == 302 and header(h, "Location") == ["/login?registered=1"])
s, b, h = req("POST", "/signup", good);                          check("duplicate email -> 409 message", s == 409 and "Email already registered" in b)

# --- login failures give the same message ---
s1, b1, h = req("POST", "/login", {"email": email, "password": "wrong-pass"})
s2, b2, h = req("POST", "/login", {"email": "nobody_%d@example.com" % int(time.time()), "password": "wrong-pass"})
check("wrong password -> 401", s1 == 401 and "Invalid email or password" in b1)
check("unknown email -> same 401 message", s2 == 401 and "Invalid email or password" in b2)
s, b, h = req("POST", "/login", {"email": email, "password": "SECRET1"})
check("password is case-sensitive", s == 401)

# --- login ---
s, b, h = req("POST", "/login", {"email": email, "password": "secret1"})
cookie = header(h, "Set-Cookie")
check("login -> 302 /products", s == 302 and header(h, "Location") == ["/products"])
m = re.match(r"session_id=([0-9a-f]{64}); HttpOnly; SameSite=Lax; Path=/; Max-Age=3600$", cookie[0] if cookie else "")
check("cookie: 64 hex id + HttpOnly + SameSite=Lax + Path=/ + Max-Age", m is not None, str(cookie))
sid = m.group(1) if m else "x"
check("session id not in Location URL", sid not in "".join(header(h, "Location")))

# --- use the session (cookie among other cookies) ---
c1 = "a=1; session_id=%s; b=2" % sid
s, b, h = req("GET", "/sessions", cookie=c1)
check("/sessions with session among many cookies", s == 200 and "My Sessions" in b)
check("current session highlighted", 'class="me"' in b and "(this session)" in b)
check("full session id is not shown in page", sid not in b)
s, b, h = req("GET", "/products", cookie=c1)
check("nav bar shows name + Logout", "Hello, Test User" in b and 'action="/logout"' in b)
s, b, h = req("GET", "/products")
check("anonymous nav shows Sign in | Sign up", 'href="/login"' in b and 'href="/signup"' in b and "Hello, guest" in b)

# --- not logged in ---
s, b, h = req("GET", "/sessions");                               check("/sessions without cookie -> 302 /login", s == 302 and header(h, "Location") == ["/login"])
s, b, h = req("GET", "/sessions", cookie="session_id=" + "0" * 64)
check("forged session id -> 302 /login", s == 302)
s, b, h = req("GET", "/sessions", cookie="session_id=' OR 1=1 --")
check("junk session id -> 302 /login", s == 302)

# --- logout ---
s, b, h = req("GET", "/logout", cookie=c1);                      check("GET /logout -> 405 with Allow: POST", s == 405 and header(h, "Allow") == ["POST"])
s, b, h = req("GET", "/sessions", cookie=c1);                    check("session still valid after failed GET /logout", s == 200)
s, b, h = req("POST", "/logout", cookie=c1)
cleared = header(h, "Set-Cookie")
check("POST /logout -> 302 /products + cookie Max-Age=0", s == 302 and header(h, "Location") == ["/products"]
      and cleared and "Max-Age=0" in cleared[0], str(cleared))
s, b, h = req("GET", "/sessions", cookie=c1);                    check("old cookie no longer works", s == 302)

# --- login again: history shows both sessions ---
s, b, h = req("POST", "/login", {"email": email, "password": "secret1"})
sid2 = re.match(r"session_id=([0-9a-f]{64})", header(h, "Set-Cookie")[0]).group(1)
check("new login gets a different session id", sid2 != sid)
s, b, h = req("GET", "/sessions", cookie="session_id=" + sid2)
check("history lists logged-out and active sessions", "Logged out" in b and "Active" in b, "")

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
