# test_cart.py - end-to-end test of the cart pages (Step 8) against a RUNNING server.
#
# NOTE: this CREATES two users (cart_a_<time>@example.com, cart_b_<time>@example.com) and cart rows in the
# database the server is connected to. It never reads or writes db.conf. Start the server yourself, then:
#     python tests\test_cart.py
import concurrent.futures, http.client, re, sys, time
from urllib.parse import urlencode

HOST, PORT = "127.0.0.1", 8080
fails = 0

def req(method, path, form=None, cookie=None):
    c = http.client.HTTPConnection(HOST, PORT, timeout=15)
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
    email = "cart_%s_%d@example.com" % (tag, int(time.time()))
    req("POST", "/signup", {"name": "Cart " + tag, "email": email, "password": "secret1"})
    s, b, h = req("POST", "/login", {"email": email, "password": "secret1"})
    return "session_id=" + re.match(r"session_id=([0-9a-f]{64})", header(h, "Set-Cookie")[0]).group(1)

def cart_count(cookie):
    s, b, h = req("GET", "/cart", cookie=cookie)
    m = re.search(r'class="badge" title="\d+ items?">(\d+)</span>', b)       # the cart badge in the navbar
    return int(m.group(1)) if m else -1

def qty_in_cart(cookie, pid):
    s, b, h = req("GET", "/cart", cookie=cookie)
    m = re.search(r'name="product_id" value="%s"><input type="number" name="qty" value="(\d+)"' % pid, b)
    return int(m.group(1)) if m else 0

A = signup_login("a")
B = signup_login("b")

# --- login required ---
for method, path in [("GET", "/cart"), ("POST", "/cart/add"), ("POST", "/cart/update"), ("POST", "/cart/remove")]:
    s, b, h = req(method, path, {"product_id": "1"} if method == "POST" else None)
    check("%s %s without login -> 302 /login" % (method, path), s == 302 and header(h, "Location") == ["/login"])
s, b, h = req("GET", "/cart/add", cookie=A);   check("GET /cart/add -> 405", s == 405)

# --- empty cart ---
s, b, h = req("GET", "/cart", cookie=A)
check("empty cart page", s == 200 and "Your cart is empty" in b and 'title="0 items">0</span>' in b)

# --- add ---
s, b, h = req("POST", "/cart/add", {"product_id": "1"}, cookie=A)
check("add product 1 (qty default 1) -> 302 /cart", s == 302 and header(h, "Location") == ["/cart"])
check("nav shows the cart badge: 1 item", cart_count(A) == 1 and 'title="1 item">1</span>' in req("GET", "/cart", cookie=A)[1])
req("POST", "/cart/add", {"product_id": "1", "qty": "4"}, cookie=A)
check("adding the same product again adds up (1 + 4 = 5)", qty_in_cart(A, "1") == 5, str(qty_in_cart(A, "1")))
req("POST", "/cart/add", {"product_id": "1", "qty": "10"}, cookie=A)
check("total is capped at MAX_CART_QTY (10)", qty_in_cart(A, "1") == 10, str(qty_in_cart(A, "1")))

# --- validation: 400 with a clear message ---
for form, what in [({"product_id": "abc"}, "product_id=abc"), ({"product_id": ""}, "empty product_id"),
                   ({"product_id": "0"}, "product_id=0"), ({"product_id": "1", "qty": "0"}, "qty=0"),
                   ({"product_id": "1", "qty": "11"}, "qty=11"), ({"product_id": "1", "qty": "-1"}, "qty=-1"),
                   ({"product_id": "1", "qty": "abc"}, "qty=abc"), ({"product_id": "1", "qty": "1.5"}, "qty=1.5")]:
    s, b, h = req("POST", "/cart/add", form, cookie=A)
    check("add with " + what + " -> 400", s == 400 and "<h1>" in b, str(s))
s, b, h = req("POST", "/cart/add", {"product_id": "999999"}, cookie=A)
check("add unknown product -> 404 'Product not found'", s == 404 and "Product not found" in b, str(s))

# --- update / remove ---
req("POST", "/cart/update", {"product_id": "1", "qty": "3"}, cookie=A)
check("update qty to 3", qty_in_cart(A, "1") == 3)
s, b, h = req("POST", "/cart/update", {"product_id": "1", "qty": "11"}, cookie=A)
check("update qty 11 -> 400", s == 400)
s, b, h = req("POST", "/cart/update", {"product_id": "2", "qty": "1"}, cookie=A)
check("update an item that is not in the cart -> 404", s == 404, str(s))
req("POST", "/cart/add", {"product_id": "2", "qty": "2"}, cookie=A)
req("POST", "/cart/update", {"product_id": "2", "qty": "0"}, cookie=A)
check("update qty 0 removes the item", qty_in_cart(A, "2") == 0)
req("POST", "/cart/remove", {"product_id": "1"}, cookie=A)
check("remove product 1 -> cart empty again", cart_count(A) == 0)
s, b, h = req("POST", "/cart/remove", {"product_id": "1"}, cookie=A)
check("removing twice is fine (302)", s == 302)

# --- total uses the DB price, form fields cannot change price or cart ---
req("POST", "/cart/add", {"product_id": "1", "qty": "2", "price": "0.01", "cart_id": "1"}, cookie=A)
s, b, h = req("GET", "/cart", cookie=A)
check("forged 'price' field ignored (Smart LED Bulb is ₹499, 2 x = ₹998)",
      "₹499<" in b and "₹998<" in b and "0.01" not in b)
check("forged 'cart_id' field ignored: user B's cart is still empty", cart_count(B) == 0 and "empty" in req("GET", "/cart", cookie=B)[1])
req("POST", "/cart/update", {"product_id": "1", "qty": "1", "cart_id": "1"}, cookie=B)
check("user B cannot touch user A's cart through update", qty_in_cart(A, "1") == 2)

# --- many requests at once on a small connection pool: nothing lost, nothing hangs ---
req("POST", "/cart/remove", {"product_id": "1"}, cookie=A)
with concurrent.futures.ThreadPoolExecutor(8) as ex:
    codes = list(ex.map(lambda i: req("POST", "/cart/add", {"product_id": "1", "qty": "1"}, cookie=A)[0], range(8)))
check("8 parallel adds of 1 all succeed", codes == [302] * 8, str(codes))
check("...and the quantity is exactly 8 (no lost update)", qty_in_cart(A, "1") == 8, str(qty_in_cart(A, "1")))

# --- product pages have the button ---
s, b, h = req("GET", "/products")
check("product cards have an 'Add to cart' POST form", 'action="/cart/add"' in b and "Add to cart" in b)
s, b, h = req("GET", "/product?id=1")
check("product page has the add form with qty", 'action="/cart/add"' in b and 'name="qty"' in b)

# --- "Add to cart" returns to the same page with a message (Step 8b) ---
req("POST", "/cart/remove", {"product_id": "1"}, cookie=A)
s, b, h = req("GET", "/products?cat=1&q=bulb", cookie=A)
check("cards carry a hidden 'return' = current path + query",
      'name="return" value="/products?cat=1&amp;q=bulb"' in b, "")
s, b, h = req("POST", "/cart/add", {"product_id": "1", "return": "/products?cat=1&q=bulb"}, cookie=A)
check("add redirects back to the page + added=1", s == 302 and header(h, "Location") == ["/products?cat=1&q=bulb&added=1"],
      str(header(h, "Location")))
s, b, h = req("GET", "/products?cat=1&q=bulb&added=1", cookie=A)
check("green message with product name and a link to the cart",
      'class="flash ok"' in b and "added to cart" in b and 'href="/cart"' in b)
check("the old added=1 is not carried into the next 'return'", 'name="return" value="/products?cat=1&amp;q=bulb"' in b)
check("nav count is already updated on that page", 'title="1 item">1</span>' in b)
s, b, h = req("POST", "/cart/add", {"product_id": "1", "return": "/product?id=1&added=7"}, cookie=A)
check("an old added= in 'return' is replaced", header(h, "Location") == ["/product?id=1&added=1"], str(header(h, "Location")))
s, b, h = req("GET", "/product?id=1&added=1", cookie=A)
check("product page shows the message too", 'class="flash ok"' in b and 'name="return" value="/product?id=1"' in b)

# open redirect: evil 'return' values must fall back to /products
for evil in ["//evil.com", "https://evil.com", "/\\evil.com", "", "javascript:alert(1)", "/a b", "/x\r\nSet-Cookie: pwn=1"]:
    s, b, h = req("POST", "/cart/add", {"product_id": "1", "return": evil}, cookie=A)
    loc = header(h, "Location")
    check("return=%r -> /products?added=1" % evil, s == 302 and loc == ["/products?added=1"], str(loc))
    check("   ...and no injected header", not header(h, "Set-Cookie") or "pwn" not in "".join(header(h, "Set-Cookie")))

# MAX_CART_QTY cap: "Maximum 10 per item" instead of "added" (Step 9)
req("POST", "/cart/remove", {"product_id": "1"}, cookie=A)
s, b, h = req("POST", "/cart/add", {"product_id": "1", "qty": "8", "return": "/products"}, cookie=A)
check("8 of 10: normal 'added' redirect", header(h, "Location") == ["/products?added=1"], str(header(h, "Location")))
s, b, h = req("POST", "/cart/add", {"product_id": "1", "qty": "5", "return": "/products"}, cookie=A)
check("8 + 5 > 10: redirect carries cart_err=max", header(h, "Location") == ["/products?added=1&cart_err=max"],
      str(header(h, "Location")))
s, b, h = req("GET", "/products?added=1&cart_err=max", cookie=A)
check("red 'Maximum 10 per item' message, not 'added to cart'",
      'class="flash err"' in b and "Maximum 10 per item" in b and "added to cart" not in b)
check("quantity really is capped at 10", qty_in_cart(A, "1") == 10)
req("POST", "/cart/remove", {"product_id": "1"}, cookie=A)

# message parameters cannot be abused
s, b, h = req("GET", "/products?added=abc", cookie=A);     check("added=abc shows no message", 'class="flash' not in b)
s, b, h = req("GET", "/products?added=999999", cookie=A);  check("added=<unknown id> shows no message", 'class="flash' not in b)
s, b, h = req("GET", "/products?added=1&cart_err=<script>", cookie=A)
check("cart_err is only compared with fixed codes (no reflection)", "<script>" not in b)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
