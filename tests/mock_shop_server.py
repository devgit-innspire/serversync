# mock_shop_server.py - a tiny FAKE shop (no database, no db.conf) that answers the few URLs oversell_test.ps1 uses,
# so the script's own logic (parallel start, summary, cleanup) can be tested without touching the real server.
#     python tests\mock_shop_server.py [port] [stock]        (default port 8099, stock 3)
# Exactly `stock` checkouts get 302; the rest get 409, like the real server should.
import http.server, socketserver, sys, threading, uuid
from urllib.parse import urlparse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
stock = int(sys.argv[2]) if len(sys.argv) > 2 else 3
lock = threading.Lock()
users = {}            # session id -> email
carts = {}            # session id -> True
orders = 0

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body="", headers=None):
        data = body.encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def session(self):
        for part in self.headers.get("Cookie", "").split(";"):
            if part.strip().startswith("session_id="):
                return part.strip()[len("session_id="):]
        return None

    def do_GET(self):
        global stock
        p = urlparse(self.path).path
        if p == "/":
            return self.send(200, "<html>home</html>")
        if p == "/product":
            with lock:
                s = stock
            return self.send(200, "Out of stock" if s == 0 else '<span class="stock in" title="%d in stock">In stock</span>' % s)
        self.send(404, "nf")

    def do_POST(self):
        global stock, orders
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode()
        p = urlparse(self.path).path
        if p == "/signup":
            return self.send(302, "", {"Location": "/login?registered=1"})
        if p == "/login":
            email = [x.split("=")[1] for x in body.split("&") if x.startswith("email=")][0]
            sid = uuid.uuid4().hex + uuid.uuid4().hex
            users[sid] = email
            return self.send(302, "", {"Location": "/products",
                                       "Set-Cookie": "session_id=%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=3600" % sid})
        if p == "/cart/add":
            if self.session() not in users:
                return self.send(302, "", {"Location": "/login"})
            carts[self.session()] = True
            return self.send(302, "", {"Location": "/products?added=2"})
        if p == "/checkout":
            if self.session() not in users:
                return self.send(302, "", {"Location": "/login"})
            with lock:                       # the real server uses row locks; the mock uses a mutex
                if stock > 0:
                    stock -= 1
                    orders += 1
                    return self.send(302, "", {"Location": "/order?id=%d" % orders})
            return self.send(409, "Only 0 left")
        self.send(404, "nf")

class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    request_queue_size = 64

Server(("127.0.0.1", PORT), Handler).serve_forever()
