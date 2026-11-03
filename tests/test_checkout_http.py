# test_checkout_http.py - the checkout / orders PAGES (Step 9) against a RUNNING server.
# (The transaction itself is tested by test_checkout.py on a throwaway database.)
#
# NOTE: this CREATES two users and one real order (it reduces product 1's stock by 2) in the database the server
# uses. It never touches db.conf.
#     python tests\test_checkout_http.py
import http.client, re, sys, time
from urllib.parse import urlencode

HOST, PORT = "127.0.0.1", 8080
fails = 0

def req(method, path, form=None, cookie=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=20)
    headers, body = {}, None
    if form is not None:
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

def header(h, name):
    return [v for k, v in h if k.lower() == name.lower()]

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def signup_login(tag):
    email = "co_%s_%d@example.com" % (tag, int(time.time()))
    req("POST", "/signup", {"name": "Co " + tag, "email": email, "password": "secret1"})
    s, b, h = req("POST", "/login", {"email": email, "password": "secret1"})
    return "session_id=" + re.match(r"session_id=([0-9a-f]{64})", header(h, "Set-Cookie")[0]).group(1)

A, B = signup_login("a"), signup_login("b")

# --- login required ---
for method, path in [("GET", "/checkout"), ("POST", "/checkout"), ("GET", "/orders"), ("GET", "/order?id=1")]:
    s, b, h = req(method, path, {} if method == "POST" else None)
    check("%s %s without login -> 302 /login" % (method, path), s == 302 and header(h, "Location") == ["/login"])

# --- empty cart ---
s, b, h = req("GET", "/checkout", cookie=A);  check("GET /checkout with empty cart", s == 200 and "cart is empty" in b)
s, b, h = req("POST", "/checkout", {}, cookie=A)
check("POST /checkout with empty cart -> 409 message, no order", s == 409 and "Your cart is empty" in b, str(s))
s, b, h = req("GET", "/orders", cookie=A);    check("no orders yet", s == 200 and "No orders yet" in b)
s, b, h = req("GET", "/cart", cookie=A);      check("cart page has no Checkout button when empty", 'action="/checkout"' not in b)

# --- confirm page, then place the order ---
req("POST", "/cart/add", {"product_id": "1", "qty": "2"}, cookie=A)          # Smart LED Bulb, Rs 499.00 each
s, b, h = req("GET", "/cart", cookie=A);      check("cart page has a Checkout button", 'action="/checkout"' in b and "Checkout" in b)
s, b, h = req("GET", "/checkout", cookie=A)
check("confirm page shows the DB total (998.00) and a POST 'Place order' form",
      s == 200 and "₹998<" in b and 'method="post" action="/checkout"' in b and "Place order" in b)
s, b, h = req("POST", "/checkout", {"total": "0.01", "cart_id": "1", "price": "0.01"}, cookie=A)   # forged fields
loc = header(h, "Location")
check("POST /checkout -> 302 /order?id=N (forged fields ignored)", s == 302 and len(loc) == 1 and re.fullmatch(r"/order\?id=\d+", loc[0]), str(loc))
oid = loc[0].split("=")[1] if loc else "0"

s, b, h = req("GET", "/order?id=" + oid, cookie=A)
check("order page: PLACED, COD (PENDING payment), total 998.00, the item",
      s == 200 and "PLACED" in b and "COD" in b and "PENDING" in b and "₹998<" in b and "Smart LED Bulb" in b, str(s))
s, b, h = req("GET", "/orders", cookie=A)
check("order shows up in /orders", s == 200 and ('/order?id=%s"' % oid) in b and "998.00" in b)
s, b, h = req("GET", "/cart", cookie=A)
check("cart is empty again after checkout (new ACTIVE cart)", "Your cart is empty" in b and 'title="0 items">0</span>' in b)
s, b, h = req("POST", "/checkout", {}, cookie=A)
check("second POST /checkout (double submit / refresh) -> 409 'cart is empty', no second order", s == 409, str(s))
s, b, h = req("GET", "/orders", cookie=A)
check("still exactly one order", b.count("/order?id=") == 1, str(b.count("/order?id=")))

# --- ownership: another user's order must look like it does not exist ---
s, b, h = req("GET", "/order?id=" + oid, cookie=B);   check("user B cannot open user A's order -> 404", s == 404, str(s))
s, b, h = req("GET", "/orders", cookie=B);            check("user B's order list has no orders", "No orders yet" in b)
s, b, h = req("GET", "/order?id=999999", cookie=A);   check("unknown order id -> 404", s == 404)
s, b, h = req("GET", "/order?id=abc", cookie=A);      check("order id not a number -> 400", s == 400)
s, b, h = req("GET", "/order", cookie=A);             check("no order id -> 400", s == 400)

# --- stock check at checkout: a cart quantity above stock is refused and rolled back ---
# (needs a product with small stock: product 15 'Clear Phone Case' usually has plenty, so only run if stock < 10)
s, b, h = req("GET", "/product?id=13", cookie=B)
m = re.search(r'title="(\d+) in stock"|Only (\d+) left', b)         # the badge: "In stock" (tooltip has the number) or "Only N left"
if m:
    m = re.match(r"(\d+)", m.group(1) or m.group(2))
if m and int(m.group(1)) < 10:
    stock = int(m.group(1))
    req("POST", "/cart/add", {"product_id": "13", "qty": str(stock)}, cookie=B)
    req("POST", "/cart/add", {"product_id": "13", "qty": "1"}, cookie=B)
    s, b, h = req("POST", "/checkout", {}, cookie=B)
    check("quantity above stock -> 409 'Only N of <name> available'", s == 409 and "available" in b, str(s))
else:
    print("SKIP stock check (product 13 has 10+ in stock)")

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
