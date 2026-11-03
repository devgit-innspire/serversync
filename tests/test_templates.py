# test_templates.py - Part 3: every page now comes from templates/*.html. Renders ALL pages through the real route()
# (src/pages.cpp) with fake database answers and checks status codes, redirects, forms, field names, escaping, that no
# template is missing a value, and that no HTML is left in the .cpp files. No database, no server, no db.conf.
#     python tests\test_templates.py
import glob, os, re, shutil, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")      # the checks print the rupee sign and emoji

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def read(p):
    with open(os.path.join(ROOT, p), encoding="utf-8") as f:
        return f.read()

# ---------- static lint of the sources ----------
tmpl_files = {os.path.basename(f)[:-5] for f in glob.glob(os.path.join(ROOT, "templates", "*.html"))}
src = {os.path.basename(f): open(f, encoding="utf-8").read() for f in glob.glob(os.path.join(ROOT, "src", "*.cpp"))}
used = set(re.findall(r'render\("([a-z_]+)"', src["pages.cpp"]))
check("every render(\"name\") in pages.cpp has a templates/name.html", used <= tmpl_files, str(sorted(used - tmpl_files)))
check("no orphan templates: every templates/*.html is used by the code", tmpl_files <= used, str(sorted(tmpl_files - used)))
req_list = set(re.findall(r'"([a-z_]+)"', re.search(r"REQUIRED_TEMPLATES\[\] = \{(.*?)\};", src["pages.cpp"], re.S).group(1)))
check("REQUIRED_TEMPLATES lists exactly the templates in use (server refuses to start without them)", req_list == tmpl_files,
      str(sorted(req_list ^ tmpl_files)))
html_literals = []
for name, text in src.items():
    for i, line in enumerate(text.splitlines(), 1):
        code = line.split("//")[0]
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', code):
            if re.search(r"<[a-zA-Z/!]", lit) and not (name == "pages.cpp" and "&lt;" in lit):
                html_literals.append("%s:%d %s" % (name, i, lit[:50]))
check("no HTML tags left in any string in src/*.cpp (only the escaping table in html_escape)", not html_literals, "; ".join(html_literals[:4]))
big = [n for n, t in src.items() if len(re.findall(r'<(?:div|table|form|h1|p|ul|tr|td)\b', t)) > 0]
check("no <div>/<table>/<form>/<h1>/<p>... markup anywhere in the .cpp files", not big, str(big))
check("old inline <style> is gone", "<style" not in src["pages.cpp"] and "STYLE" not in src["pages.cpp"])
check("owners: server.cpp = Aradhana, pages.cpp = Ashutosh", "Owner: Aradhana" in src["server.cpp"] and "Owner: Ashutosh" in src["pages.cpp"])
for f in sorted(tmpl_files):
    t = read(os.path.join("templates", f + ".html"))
    bad = re.findall(r"\{\{[^{}]*\{\{|\}\}[^{}]*\}\}\}|\{\{\{[^}]*\}\}(?!\})", t)
    if bad:
        check("template %s has balanced braces" % f, False, str(bad))
check("every template has balanced {{ }} / {{{ }}} (no stray braces)", True)

# ---------- render every page ----------
work = tempfile.mkdtemp(prefix="serversync_render_")
try:
    exe = os.path.join(work, "pages_render_check.exe")
    import time
    for attempt in range(6):            # g++ sometimes runs out of memory when the PC is busy: just try again
        r = subprocess.run(["g++", "-std=c++20", "-Wall", "-o", exe, os.path.join(ROOT, "tests", "pages_render_check.cpp"),
                            "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(BASE, "include"), "-lws2_32", "-lbcrypt",
                            "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
        if r.returncode == 0 or "out of memory" not in r.stderr:
            break
        time.sleep(5)
    if r.returncode != 0:
        print(r.stderr[:3000])
        sys.exit("build failed")
    p = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", cwd=ROOT)
    raw_out = p.stdout
    if p.returncode != 0:
        print("harness failed", p.returncode, p.stdout[-300:], p.stderr[-300:])
        sys.exit(1)
    pages = {}
    for m in re.finditer(r"=== (\w+)\nSTATUS (\d+) SENT (\d+)\nTYPE ([^\n]*)\nEXTRA ([^\n]*)\nPROBLEMS (\d+) ?([^\n]*)\nBODY\n(.*?)\n=== END", p.stdout, re.S):
        pages[m[1]] = dict(status=int(m[2]), sent=int(m[3]), type=m[4], extra=m[5], problems=int(m[6]), why=m[7], body=m[8])
finally:
    shutil.rmtree(work, ignore_errors=True)

def pg(n): return pages[n]
def is_admin_case(k): return k.startswith(("admin", "ap_", "ar_", "at_", "ac_", "ao_", "as_", "au_", "lg_")) or (k.startswith("sx_") and k != "sx_customer_page")      # these cases run as an ADMIN (or hit /admin)
def has(n, *needles): return all(x in pg(n)["body"] for x in needles)
def hasnt(n, *needles): return not any(x in pg(n)["body"] for x in needles)

check("all 250+ page cases rendered", len(pages) >= 250, str(len(pages)))
bad = {k: v["why"] for k, v in pages.items() if v["problems"]}
check("NO page asked a template for a value the code did not give (missing keys)", not bad, str(bad))
check("status returned by route() equals what was sent, for every page", all(v["status"] == v["sent"] for v in pages.values()))
check("every page is a complete HTML document from layout.html (except redirects, json)",
      all(v["body"].startswith("<!doctype html>") for k, v in pages.items() if v["status"] != 302 and k != "metrics_json"),
      str([k for k, v in pages.items() if v["status"] != 302 and k != "metrics_json" and not v["body"].startswith("<!doctype html>")]))
check("every page links /static/style.css and the favicon", all('href="/static/style.css"' in v["body"] and 'href="/static/favicon.svg"' in v["body"]
      for k, v in pages.items() if v["status"] != 302 and k != "metrics_json" and not is_admin_case(k)))

# status codes and redirects exactly as before
expect = {"home": 200, "home_user": 200, "home_added": 200, "home_nodb": 200, "login_page_guest": 200, "404": 404, "test": 200, "dbtest": 200, "metrics": 200, "metrics_json": 200, "metrics_403": 403,
          "products": 200, "products_user": 200, "products_max": 200, "products_min_max": 200, "products_bad_price": 400, "products_cat_q": 200, "products_bad_cat": 400, "products_empty": 200, "product": 200,
          "product_soldout": 200, "product_user_plenty": 200, "product_404": 404, "product_400": 400, "signup_get": 200, "signup_invalid": 400,
          "signup_duplicate": 409, "signup_ok": 302, "login_get": 200, "login_registered": 200, "login_wrong": 401,
          "cart": 200, "cart_guest": 302, "cart_method": 405, "cart_empty": 200, "cart_add_max": 302, "cart_add_bad": 400,
          "checkout_confirm": 200, "checkout_confirm_empty": 200, "checkout_conflict": 409, "checkout_busy": 503,
          "checkout_ok": 302, "orders": 200, "orders_empty": 200, "order": 200, "order_404": 404, "order_400": 400,
          "sessions": 200, "sessions_guest": 302, "admin": 200, "admin_guest": 302, "admin_customer": 403, "admin_customer_deep": 403,
          "admin_guest_deep": 302, "admin_post": 405, "admin_logs": 200, "admin_404": 404, "admin_lookalike": 404,
          "home_admin": 200, "admin_nodb": 500, "admin_empty": 200,
          "cart_hidden": 200, "admin_customer_post": 403, "admin_guest_post": 302,
          "ao_list": 200, "ao_list_filtered": 200, "ao_list_badstatus": 400, "ao_list_badpage": 400, "ao_list_badpage2": 400, "ao_list_post": 405,
          "ao_page1": 200, "ao_page2": 200, "ao_empty": 200, "ao_detail_PLACED": 200, "ao_detail_SHIPPED": 200, "ao_detail_DELIVERED": 200,
          "ao_detail_CANCELLED": 200, "ao_detail_404": 404, "ao_detail_badid": 400, "ao_detail_post": 405, "ao_detail_guest": 302, "ao_detail_customer": 403,
          "as_ship": 302, "as_deliver": 302, "as_cancel": 302, "as_notallowed": 400, "as_404": 404, "as_409": 409, "as_503": 503, "as_500": 500,
          "as_bad_to": 302, "as_bad_to2": 302, "as_bad_id": 302, "as_bad_from": 302, "as_get": 405, "as_customer": 403, "as_guest": 302,
          "oc_PLACED": 200, "oc_SHIPPED": 200, "oc_DELIVERED": 200, "oc_CANCELLED": 200, "oc_flash": 200, "oc_cancel_ok": 302, "oc_cancel_400": 400,
          "oc_cancel_404": 404, "oc_cancel_503": 503, "oc_cancel_500": 500, "oc_cancel_guest": 302, "oc_cancel_get": 405, "oc_cancel_badid": 400}
check("status codes are unchanged for all %d pages" % len(expect), all(pg(k)["status"] == v for k, v in expect.items()),
      str({k: pg(k)["status"] for k, v in expect.items() if pg(k)["status"] != v}))
check("redirects keep their targets: signup -> /login?registered=1, checkout -> /order?id=42, guest cart/sessions -> /login",
      "Location: /login?registered=1" in pg("signup_ok")["extra"] and "Location: /order?id=42" in pg("checkout_ok")["extra"] and
      "Location: /login" in pg("cart_guest")["extra"] and "Location: /login" in pg("sessions_guest")["extra"])
check("cart/add keeps its redirect with added= and cart_err=max", "Location: /products?cat=1&added=7&cart_err=max" in pg("cart_add_max")["extra"],
      pg("cart_add_max")["extra"])
check("redirect body comes from the plain template", pg("signup_ok")["body"] == '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"></head><body>Redirecting...</body></html>')
check("405 sends the Allow header and the error page", "Allow: POST" in pg("cart_method")["extra"] and has("cart_method", "<h1>Method Not Allowed</h1>"))
check("503 checkout keeps Retry-After: 2", "Retry-After: 2" in pg("checkout_busy")["extra"])
check("metrics are not cacheable and refresh via meta tag", "Cache-Control: no-store" in pg("metrics")["extra"] and
      has("metrics", '<meta http-equiv="refresh" content="2">') and hasnt("metrics", "<script"))
check("metrics.json is JSON", pg("metrics_json")["type"] == "application/json" and pg("metrics_json")["body"].startswith("{"))

# navigation / flash area
RS = "\u20b9"
check("navbar row 1: logo 'Server'+'Sync', search form (category select + text + button) doing GET /products?cat=&q=",
      has("home", '<a class="logo" href="/">Server<span>Sync</span></a>', '<form class="search" action="/products" method="get">',
          '<select name="cat"', '<input name="q"', '<button type="submit"'))
check("search select: 'All' + every category with its id; the selected one is marked",
      has("home", '<option value="" selected>All</option>', '<option value="1" >Audio</option>', '<option value="2" >Cameras</option>') and
      has("products_cat_q", '<option value="1" selected>Audio</option>', '<option value="" >All</option>') and has("products_cat_q", 'value="lap"'))
check("guest navbar: 'Hello, guest / Sign in' and 'New here? / Sign up' open the popups (and stay real /login /signup links)",
      has("home", 'href="/login" data-modal="loginModal"><small>Hello, guest</small><strong>Sign in</strong>',
          'href="/signup" data-modal="signupModal"><small>New here?</small><strong>Sign up</strong>') and hasnt("home", 'class="logout"', "Hello, Ann"))
check("cart icon with an orange count badge (guest sees 0)", has("home", '<span class="badge" title="0 items">0</span>', 'href="/cart"'))
check("logged in: 'Hello, <name>' escaped, Orders, My Sessions, Logout POST form, cart badge 3 items",
      has("cart", "Hello, Ann &lt;b&gt;", '<span class="badge" title="3 items">3</span>', 'href="/orders"', 'href="/sessions"',
          'method="post" action="/logout"') and hasnt("cart", "Ann <b>", "Sign in", "Sign up"))
check("navbar row 2: All products, 'Under \u20b91,000' -> /products?max=1000, then every category with its emoji",
      has("home", '<a class="all" href="/products">', 'href="/products?max=1000">&#128293; Under ' + RS + '1,000</a>',
          '<a href="/products?cat=1">\U0001F3A7 Audio</a>', '<a href="/products?cat=2">\U0001F4F7 Cameras</a>'))
check("the navbar is on every page (also errors and /metrics, which never touch the database)",
      all('<div class="subnav">' in pg(k)["body"] for k in ("404", "products_bad_cat", "product_404", "metrics", "metrics_403", "cart", "order")))
check("flash area: signup error is a red box above the form, values stay (escaped)",
      has("signup_invalid", '<div class="flash err">Please enter a valid email address.</div>', 'value="Bob &lt;i&gt;"') and
      pg("signup_invalid")["body"].index("flash err") < pg("signup_invalid")["body"].index("<h1>Sign up</h1>"))
check("duplicate email -> 409 red 'Email already registered.'", has("signup_duplicate", '<div class="flash err">Email already registered.</div>'))
check("login wrong -> 401 same generic message, email escaped and kept",
      has("login_wrong", '<div class="flash err">Invalid email or password.</div>', 'value="a&lt;b&gt;@x.com"'))
check("login?registered=1 -> green 'Account created.'", has("login_registered", '<div class="flash ok">Account created. Please log in.</div>'))
check("login/signup forms keep their action and field names",
      has("login_get", 'method="post" action="/login"', 'name="email"', 'name="password"') and
      has("signup_get", 'method="post" action="/signup"', 'name="name"', 'name="email"', 'name="password"'))

# products
check("product card: pastel emoji box, escaped name, one-line description, \u20b9 price with comma, 'Only 5 left', Add to cart form (product_id + return)",
      has("products_user", '<span class="emoji">\U0001F3A7</span>', 'href="/product?id=7">Gadget &lt;b&gt;x&lt;/b&gt;</a>', '<p class="pdesc">Nice &lt;i&gt;thing&lt;/i&gt; &amp; more</p>',
          '<div class="price">' + RS + '1,499</div>', '<span class="stock low">Only 5 left</span>', 'action="/cart/add"',
          '<input type="hidden" name="product_id" value="7"><input type="hidden" name="return" value="/products"><button class="buy">Add to cart</button>') and
      hasnt("products_user", "<b>x</b>", "<i>thing</i>", "Rs "))
check("stock badges: 'In stock' (green) above 5, 'Only N left' (orange) at 5 or less, 'Out of stock' disabled button at 0",
      has("product_user_plenty", '<span class="stock in" title="40 in stock">In stock</span>') and hasnt("product_user_plenty", "Only") and
      has("products_user", "Only 5 left") and has("products_user", "<button disabled>Out of stock</button>") and
      pg("products_user")["body"].count('action="/cart/add"') == 1)
check("guest cards: 'Sign in to buy' opens the login popup, there is no add form at all",
      has("products", '<a class="buy" href="/login" data-modal="loginModal">Sign in to buy</a>') and hasnt("products", 'action="/cart/add"') and
      pg("products")["body"].count("Sign in to buy") == 1)
check("paise are shown only when needed (\u20b999.50 and \u20b910)", has("product_user_plenty", RS + "99.50") and has("products_user", '>' + RS + '10<'))
hues = re.findall(r'--h:(\d+)', pg("products_user")["body"])
check("hue is per category (different categories differ, from a number 0-359)", len(set(hues)) == 2 and all(0 <= int(h) < 360 for h in hues), str(hues))
check("sidebar: All + each category with emoji, 'on' for the chosen one; price form keeps cat and q as hidden fields",
      has("products_cat_q", '<a class="side-cat on" href="/products?cat=1">\U0001F3A7 Audio</a>', '<a class="side-cat" href="/products?cat=2">\U0001F4F7 Cameras</a>',
          '<a class="side-cat" href="/products">', '<input type="hidden" name="cat" value="1">', '<input type="hidden" name="q" value="lap">',
          'class="price-form" method="get" action="/products"', 'name="min"', 'name="max"') and
      has("products", '<a class="side-cat on" href="/products">'))
check("heading and count: 'All products' / category emoji + name; counts",
      has("products", "<h1>All products</h1>", "2 products") and has("products_cat_q", "<h1>\U0001F3A7 Audio</h1>", 'matching &quot;lap&quot;'))
check("/products?max=1000: price filter shown in the form and in the note", has("products_max", 'name="max" placeholder="Max" min="0" value="1000"', "up to " + RS + "1,000"))
check("/products?min&max with cat and q all together", has("products_min_max", 'value="100"', 'value="500"', "from " + RS + "100", "up to " + RS + "500", 'name="cat" value="1"'))
check("bad price filter -> 400 error page", has("products_bad_price", "<h1>Bad price filter</h1>"))
check("empty result: 'No products found.'", has("products_empty", "No products found.", "0 products"))
check("flash after add: green box with product name, link to cart",
      has("products_added", '<div class="flash ok">&#10003; Gadget &lt;b&gt;x&lt;/b&gt; added to cart. <a href="/cart">View cart</a></div>'))
check("flash: Maximum 10 per item (red), out of stock (red), Only 5 left (red)",
      has("products_added_max", '<div class="flash err">Maximum 10 per item: Gadget') and
      has("products_added_out", '<div class="flash err">Sorry, Gadget') and
      has("products_added_stock", '<div class="flash err">Only 5 left of Gadget'))
check("flash sits in the flash area, before the page content", pg("products_added")["body"].index("flash ok") < pg("products_added")["body"].index('class="shop"'))
check("product page: h1, breadcrumb to the category, escaped description, \u20b9 price, qty form with return",
      has("product", "<h1>Gadget &lt;b&gt;x&lt;/b&gt;</h1>", 'href="/products?cat=1">Audio</a>', "Nice &lt;i&gt;thing&lt;/i&gt; &amp; more", RS + "1,499",
          '<span class="emoji">\U0001F3A7</span>', "Only 5 left", 'name="qty" value="1" min="1" max="10"', 'name="return" value="/product?id=7"', "Back to products") and
      hasnt("product", "<i>thing</i>"))
check("product page: sold-out button, flash",
      has("product_soldout", "<button disabled>Out of stock</button>", RS + "10<") and hasnt("product_soldout", 'action="/cart/add"') and
      has("product_added", 'class="flash ok"'))
check("errors: 404 'Product not found', 400 for a bad id and bad category", has("product_404", "<h1>Product not found</h1>") and
      has("product_400", "<h1>Bad product id</h1>") and has("products_bad_cat", "<h1>Bad category id</h1>"))

# cart
check("cart: rows with escaped names, update form (adjacent hidden id + qty), remove form, \u20b9 subtotals, total, checkout button",
      has("cart", '<input type="hidden" name="product_id" value="7"><input type="number" name="qty" value="2" min="0" max="10">',
          'method="post" action="/cart/update"', 'method="post" action="/cart/remove"', RS + "998<", RS + "30<", RS + "5.50",
          RS + "1,033.50", 'method="get" action="/checkout"', "Gadget &lt;b&gt;x&lt;/b&gt;") and hasnt("cart", "Rs "))
check("cart warnings: 'Only 1 left' (qty above stock) and 'Out of stock'", has("cart", '<div class="warn">Only 1 left</div>', '<div class="warn">Out of stock</div>'))
check("empty cart: 'Your cart is empty.' + Browse products", has("cart_empty", "Your cart is empty.", 'href="/products"') and
      hasnt("cart_empty", 'action="/checkout"'))
check("checkout confirm: totals, warning box for the low-stock item, POST Place order form",
      has("checkout_confirm", "<h1>Confirm your order</h1>", RS + "1,028", 'class="flash err">Low Stock: only 1 left, you have 3 in your cart.',
          'method="post" action="/checkout"', "Place order", "Cash on delivery", 'href="/cart"'))
check("checkout empty", has("checkout_confirm_empty", "Your cart is empty."))
check("checkout failure: red message box (escaped) + heading + back link; status 409",
      has("checkout_conflict", '<div class="flash err">Only 2 of &lt;b&gt;Gadget&lt;/b&gt; available</div>', "<h1>Order not placed</h1>", "Back to cart"))

# orders
check("orders list: links /order?id=N, \u20b9 totals, status pills (PLACED yellow, DELIVERED green)",
      has("orders", 'href="/order?id=7">#7</a>', RS + "998<", '<span class="pill pill-placed">PLACED</span>', '<span class="pill pill-delivered">DELIVERED</span>') and
      pg("orders")["body"].count("/order?id=") == 2)
check("orders empty: 'No orders yet.'", has("orders_empty", "No orders yet."))
check("order page: status pill, placed, payment 'COD' + status pill, item with price at purchase, total",
      has("order", "<h1>Order #7</h1>", '<span class="pill pill-placed">PLACED</span>', "2026-10-24 10:00:00",
          'COD <span class="pill pill-pending">PENDING</span>', "Gadget &lt;b&gt;x&lt;/b&gt;", RS + "499<", RS + "998<", "All orders"))
check("order 404 / 400", has("order_404", "<h1>Order not found</h1>") and has("order_400", "<h1>Invalid order id</h1>"))

# sessions
check("sessions: current row highlighted with class me + '(this session)'; pills Active / Logged out / Expired",
      has("sessions", '<tr class="me"><td>aaaaaaaa...</td>', '<span class="pill pill-active">Active</span> (this session)',
          '<span class="pill pill-loggedout">Logged out</span>', '<span class="pill pill-expired">Expired</span>') and
      pg("sessions")["body"].count('class="me"') == 1)
check("sessions: only 8 characters of each session id are shown", hasnt("sessions", "a" * 9, "b" * 9))

# test / dbtest / errors
check("/test: params and headers escaped, cookie header hidden", has("test", "<li>a = 1</li>", "<li>b = &lt;x&gt;</li>", "cookie: (hidden)", "h&lt;1&gt;") and
      hasnt("test", "SECRET"))
check("/dbtest shows result, connection, thread", has("dbtest", "<h1>Products in database</h1>", "Result: 52", "Connection: 2", "Thread: 3"))
check("403 page does not leak numbers", has("metrics_403", "<h1>403 Forbidden</h1>", "only available from 127.0.0.1") and hasnt("metrics_403", "3725", "120"))
check("404 route page", has("404", "<h1>404 Not Found</h1>"))

# ---------- home page ----------
home = pg("home")["body"]
check("home: gradient hero with the heading, short text, orange 'Start shopping' and white 'Create account' (opens signup popup)",
      has("home", '<section class="hero2">', "<h1>Everything you need, delivered to your door.</h1>", '<a class="btn-orange" href="/products">Start shopping</a>',
          '<a class="btn-white" href="/signup" data-modal="signupModal">Create account</a>', 'class="hero-art"'))
check("home (logged in): second hero button is 'My orders' instead of 'Create account'",
      has("home_user", '<a class="btn-white" href="/orders">My orders</a>') and hasnt("home_user", "Create account"))
check("home: four tiles (Cash on Delivery, Live stock, Order history, Session login)",
      has("home", "Cash on Delivery", "Live stock", "Order history", "Session login") and home.count('class="tile"') == 4)
check("home: 'Shop by category' tiles with emoji link to /products?cat=N",
      has("home", "Shop by category", '<a class="cat-tile" href="/products?cat=1"><span class="cat-emoji">\U0001F3A7</span><span>Audio</span></a>',
          '<a class="cat-tile" href="/products?cat=2"><span class="cat-emoji">\U0001F4F7</span><span>Cameras</span></a>'))
check("home: 'Budget picks under \u20b91,000' with exactly 8 product cards (the same product_card template)",
      has("home", "Budget picks under " + RS + "1,000", 'href="/products?max=1000">See all') and home.count('<article class="pcard">') == 8, str(home.count('<article class="pcard">')))
check("home: cards are the normal cards (escaped name and description, \u20b9 price, badge, coloured emoji box, Sign in to buy for guests)",
      has("home", "Featured 1 &lt;i&gt;", "Cheap thing &lt;u&gt;1&lt;/u&gt;", RS + "99<", '<span class="stock in" title="7 in stock">In stock</span>') and hasnt("home", "<i>", "<u>") and
      len(re.findall(r'class="pimg" style="--h:\d+"', home)) == 8 and home.count("Sign in to buy") == 8)
check("home (logged in): 8 'Add to cart' forms, each returning to / (hidden return=/)",
      pg("home_user")["body"].count('name="return" value="/"') == 8 and pg("home_user")["body"].count("Add to cart") == 8 and hasnt("home_user", "Sign in to buy"))
check("home: after adding, the green flash shows on the home page too",
      has("home_added", '<div class="flash ok">&#10003; Featured 1 &lt;i&gt; added to cart.'))
check("home: without a database the hero still shows (status 200), no category or budget sections",
      pg("home_nodb")["status"] == 200 and has("home_nodb", '<section class="hero2">', "Start shopping") and
      hasnt("home_nodb", "Shop by category", "Budget picks", '<article class="pcard">'))

# ---------- login / signup popups (modals) ----------
for name in ("home", "products", "404", "login_page_guest", "signup_get", "login_wrong"):
    check("guest page '%s' carries both popups, app.js and data-modal navbar links" % name,
          has(name, '<div class="modal" id="loginModal"', '<div class="modal" id="signupModal"', '<script src="/static/app.js" defer></script>',
              'href="/login" data-modal="loginModal"', 'href="/signup" data-modal="signupModal"'))
b = pg("home")["body"]
login_modal = b[b.index('id="loginModal"'):b.index('id="signupModal"')]
signup_modal = b[b.index('id="signupModal"'):]
check("login popup posts to /login with the same field names (email, password)",
      'method="post" action="/login"' in login_modal and 'name="email"' in login_modal and 'name="password"' in login_modal and 'type="password"' in login_modal)
check("signup popup posts to /signup with the same field names (name, email, password)",
      'method="post" action="/signup"' in signup_modal and all('name="%s"' % f in signup_modal for f in ("name", "email", "password")))
check("popups switch to each other with data-modal links that are real /signup and /login links (work without JS)",
      'href="/signup" data-modal="signupModal"' in login_modal and 'href="/login" data-modal="loginModal"' in signup_modal)
check("popups have a close button (x), dialog role and are hidden by default (no 'open' class in the HTML)",
      'data-close' in login_modal and 'role="dialog"' in login_modal and "&times;" in login_modal and "modal open" not in b)
for name in ("home_user", "cart", "orders", "sessions", "products_user"):
    check("logged-in page '%s' has NO popups" % name, hasnt(name, 'id="loginModal"', 'id="signupModal"', "data-modal"))
check("logged-in navbar: Logout is a POST form", has("home_user", 'method="post" action="/logout" class="inline"><button class="logout">Logout</button>'))
check("app.js is also loaded for logged-in pages (harmless, no popups there)", has("home_user", '<script src="/static/app.js" defer></script>'))
check("/metrics page has no JavaScript and no popups (it refreshes with a meta tag; its navbar links still go to /login and /signup)",
      hasnt("metrics", "<script", 'id="loginModal"', 'id="signupModal"') and has("metrics", 'href="/login"', 'href="/signup"'))
check("/login and /signup pages themselves still have their normal forms (errors / no JavaScript)",
      has("login_get", '<h1>Login</h1>') and has("signup_get", "<h1>Sign up</h1>") and has("login_wrong", '<div class="flash err">Invalid email or password.</div>'))
check("no 'Rs' prices left anywhere (the rupee sign is used)", all(not re.search(r"\bRs\b", v["body"]) for v in pages.values()))
check("every page body is valid UTF-8 text with no broken bytes (rupee sign and emoji intact)", all("\ufffd" not in v["body"] for v in pages.values()))

# ---------- style.css (light theme only) ----------
css = read(os.path.join("public", "style.css"))
css_no_comments = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
def parse_vars(block):
    return {k: v.strip() for k, v in re.findall(r"(--[\w-]+):\s*([^;]+);", block)}
root = parse_vars(re.search(r":root\s*\{(.*?)\n\}", css_no_comments, re.S).group(1))
spec = {"--nav": "#131921", "--nav2": "#232f3e", "--accent": "#f59e0b", "--hero-from": "#4f46e5", "--hero-to": "#7c3aed", "--bg": "#f3f4f6",
        "--card": "#ffffff", "--ok": "#16a34a", "--warn": "#ea580c", "--text": "#111827", "--muted": "#6b7280"}
check("css: the colour variables from the design brief are defined exactly", all(root.get(k) == v for k, v in spec.items()),
      str({k: root.get(k) for k, v in spec.items() if root.get(k) != v}))
check("css: LIGHT theme only (no dark mode, no prefers-color-scheme, no data-theme)", "prefers-color-scheme" not in css and "data-theme" not in css and "@media (prefers" not in css)
check("css: no CDN / external URLs, no @import, no web fonts, no images", not re.search(r"https?://|@import|url\(|@font-face", css_no_comments))
check("css: hero gradient uses the two hero colours", re.search(r"\.hero2\s*\{[^}]*linear-gradient\(135deg, var\(--hero-from\), var\(--hero-to\)\)", css, re.S) is not None)
check("css: popups (.modal / .modal.open / .modal-box / .modal-close), pills for PLACED / DELIVERED / CANCELLED",
      all(x in css for x in (".modal {", ".modal.open", ".modal-box", ".modal-close", ".pill-placed", ".pill-delivered", ".pill-cancelled")))
check("css: sticky header, cart badge, product grid, pastel emoji box from the --h hue variable, orange buy button",
      re.search(r"\.site-header\s*\{[^}]*position:\s*sticky", css) is not None and re.search(r"\.badge\s*\{", css) is not None and ".grid" in css and
      re.search(r"\.pimg\s*\{[^}]*hsl\(var\(--h", css, re.S) is not None and re.search(r"\.buy[^{]*\{[^}]*background:\s*var\(--accent\)", css, re.S) is not None)
check("css: responsive for mobile (max-width media queries, subnav scrolls sideways, tables scroll)",
      "@media (max-width: 700px)" in css and "@media (max-width: 420px)" in css and re.search(r"\.subnav-inner\s*\{[^}]*overflow-x:\s*auto", css, re.S) is not None
      and ".table-wrap" in css and "overflow-x: auto" in css)
check("css: disabled button style (Out of stock)", ".soldout button" in css and "not-allowed" in css)
check("css: flash boxes green and red", ".flash.ok" in css and ".flash.err" in css)

# ----- contrast of the text colours (WCAG AA 4.5:1) -----
def lum(hexcol):
    h = hexcol.lstrip("#")
    r, g, b = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    f = lambda c: c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)
def ratio(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
pairs = [("text on page", root["--text"], root["--bg"]), ("text on cards", root["--text"], root["--card"]),
         ("muted text on cards", root["--muted-text"], root["--card"]), ("muted text on page", root["--muted-text"], root["--bg"]),
         ("link on cards", root["--link"], root["--card"]), ("white on navbar", "#ffffff", root["--nav"]), ("white on subnav", "#ffffff", root["--nav2"]),
         ("navbar button text", root["--nav"], root["--accent"]), ("ok text on pill", root["--ok-text"], "#dcfce7"),
         ("warn text on badge", root["--warn-text"], "#ffedd5"), ("error text on flash", root["--err-text"], "#fef2f2"),
         ("ok text on flash", root["--ok-text"], "#f0fdf4"), ("pending pill", "#854d0e", "#fef9c3"), ("white on hero start", "#ffffff", root["--hero-from"]),
         ("hero text on hero end", "#eef2ff", root["--hero-to"]), ("hero button text", root["--hero-from"], "#ffffff"), ("muted on soft", root["--muted-text"], "#e5e7eb")]
low = [(n, round(ratio(a, b), 2)) for n, a, b in pairs if ratio(a, b) < 4.5]
check("contrast: every text/background pair is at least 4.5:1 (WCAG AA)", not low, str(low))
check("cards are clearly different from the page background (white on #f3f4f6 plus shadow)", ratio(root["--card"], root["--bg"]) >= 1.05)

# every class used by a template has a CSS rule (or is a known utility without styling)
used_classes = set()
for f in sorted(tmpl_files):
    if f.startswith("admin_"):
        continue                                    # admin templates are checked against admin.css above
    for m in re.finditer(r'class="([^"{]*)', read(os.path.join("templates", f + ".html"))):
        used_classes.update(m.group(1).split())
used_classes.update({"on", "me", "pill-placed", "pill-delivered", "pill-cancelled", "pill-pending", "pill-paid", "pill-active", "pill-loggedout", "pill-expired"})
css_classes = set(re.findall(r"\.([a-zA-Z][\w-]*)", css_no_comments))
no_rule = sorted(c for c in used_classes if c not in css_classes and c not in ("ok", "err", "inline", "box", "all", "pill-"))
check("every class used in the templates has a CSS rule", not no_rule, str(no_rule))

# ---------- admin panel (A1) ----------
adm = pg("admin")["body"]
check("admin: guest -> 302 /login, customer -> 403 (also on deep paths and unknown paths), nothing of the admin page leaks",
      "Location: /login" in pg("admin_guest")["extra"] and "Location: /login" in pg("admin_guest_deep")["extra"] and
      has("admin_customer", "<h1>403 Forbidden</h1>") and has("admin_customer_deep", "<h1>403 Forbidden</h1>") and
      hasnt("admin_customer", "admin.css", "Revenue", "side-link") and hasnt("admin_customer_deep", "admin.css", "side-link"))
check("admin: '/administrator' is not an admin route (404 from the normal router)", has("admin_lookalike", "<h1>404 Not Found</h1>") and hasnt("admin_lookalike", "admin.css"))
check("admin: POST is refused (405 + Allow: GET) after the guard", "Allow: GET" in pg("admin_post")["extra"])
check("admin layout: own sidebar + admin.css, no store navbar, no store popups",
      has("admin", 'href="/static/admin.css"', 'class="brand"', "Admin</small>", 'class="side-nav"') and hasnt("admin", "/static/style.css", 'class="topbar"', "loginModal"))
check("admin sidebar: Dashboard, Products, Categories, Orders, Users, Sessions, Logs, Back to store, Sign out (POST /logout)",
      has("admin", 'href="/admin"', 'href="/admin/products"', 'href="/admin/categories"', 'href="/admin/orders"', 'href="/admin/users"',
          'href="/admin/sessions"', 'href="/admin/logs"', "Back to store", 'method="post" action="/logout"', "Sign out"))
check("admin sidebar: only the current page is highlighted (Dashboard on /admin, Logs on /admin/logs)",
      adm.count("side-link on") == 1 and 'side-link on" href="/admin">' in adm and 'side-link on" href="/admin/logs">' in pg("admin_logs")["body"] and
      pg("admin_logs")["body"].count("side-link on") == 1)
check("admin: signed-in name is shown escaped", has("admin", "Boss &lt;i&gt;") and hasnt("admin", "Boss <i>"))
check("dashboard cards: revenue, orders, waiting, users, products, low stock, active sessions with the right numbers",
      has("admin", "Revenue (excl. cancelled)", RS + "82,672<", "Total orders", "Orders waiting (PLACED)", "Users", "Products", "Low stock (&lt;= 5)",
          "Active sessions") and bool(re.search(r'stat warn"><span class="stat-label">Orders waiting \(PLACED\)</span><span class="stat-value">7<', adm)) and
      bool(re.search(r'stat err"><span class="stat-label">Low stock[^<]*</span><span class="stat-value">1<', adm)) and
      bool(re.search(r'Total orders</span><span class="stat-value">8<', adm)) and bool(re.search(r'Products</span><span class="stat-value">52<', adm)))
check("dashboard: no warning colours when the numbers are 0", hasnt("admin_empty", "stat warn", "stat err"))
check("recent orders: id, customer (escaped), date, total with rupee sign, status pills",
      has("admin", '<td><a href="/admin/order?id=14">#14</a></td>', "Aarav &lt;b&gt;Sharma&lt;/b&gt;", "2026-10-13 19:29:39", RS + "33,892<", RS + "10.50<",
          '<span class="pill pill-placed">PLACED</span>', '<span class="pill pill-delivered">DELIVERED</span>',
          '<span class="pill pill-cancelled">CANCELLED</span>') and hasnt("admin", "<b>Sharma</b>"))
check("orders by status: counts in pills", has("admin", '<span class="pill pill-placed">PLACED: 7</span>', '<span class="pill pill-delivered">DELIVERED: 1</span>'))
check("low stock list: escaped names, stock 0 red, others yellow",
      has("admin", "Smart LED &lt;b&gt;Bulb&lt;/b&gt;", '<span class="pill pill-placed">5</span>', '<span class="pill pill-cancelled">0</span>') and hasnt("admin", "<b>Bulb</b>"))
check("dashboard with no data: friendly empty texts", has("admin_empty", "No orders yet.", "Every product has more than 5 in stock."))
check("dashboard: database error -> 500 error page (not a half page)", has("admin_nodb", "<h1>Database error</h1>") and hasnt("admin_nodb", "Revenue"))
check("store navbar: admins get an 'Admin' link to /admin, customers and guests never do",
      has("home_admin", '<a class="acct" href="/admin"><small>Manage</small><strong>Admin</strong></a>') and
      hasnt("home_user", 'href="/admin"') and hasnt("home", 'href="/admin"') and hasnt("cart", 'href="/admin"'))
check("no store page contains an admin link or admin.css (customers never see the admin area)",
      all('href="/admin' not in v["body"] and "admin.css" not in v["body"] for k, v in pages.items() if not is_admin_case(k) and k != "home_admin"))
check("admin pages are never cached (Cache-Control: no-store)", "Cache-Control: no-store" in pg("admin")["extra"])

# admin.css
acss = read(os.path.join("public", "admin.css"))
acss_nc = re.sub(r"/\*.*?\*/", "", acss, flags=re.S)
check("admin.css: dark navy sidebar, indigo #4f46e5 active link, light grey page, white cards",
      re.search(r"--navy:\s*#111827", acss) and re.search(r"--indigo:\s*#4f46e5", acss) and re.search(r"--bg:\s*#f3f4f6", acss) and
      re.search(r"\.side-link\.on\s*\{[^}]*background:\s*var\(--indigo\)", acss) and re.search(r"\.side\s*\{[^}]*background:\s*var\(--navy\)", acss) and
      re.search(r"\.card\s*\{[^}]*background:\s*var\(--card\)", acss))
check("admin.css: no CDN / external URLs / dark-mode switch, mobile layout (sidebar on top at 900px)",
      not re.search(r"https?://|@import|url\(|prefers-color-scheme", acss_nc) and "@media (max-width: 900px)" in acss)
check("admin.css: white text on the indigo link and on the navy table header is at least 4.5:1",
      lum("#ffffff") is not None and ratio("#ffffff", "#4f46e5") >= 4.5 and ratio("#ffffff", "#111827") >= 4.5 and ratio("#4b5563", "#ffffff") >= 4.5 and
      ratio("#c2410c", "#ffffff") >= 4.5 and ratio("#b91c1c", "#ffffff") >= 4.5 and ratio("#e5e7eb", "#111827") >= 4.5)
admin_classes = set()
for f in sorted(tmpl_files):
    if f.startswith("admin_") and f != "admin_nav_link":        # admin_nav_link sits in the STORE navbar (style.css)
        for m in re.finditer(r'class="([^"{]*)', read(os.path.join("templates", f + ".html"))):
            admin_classes.update(m.group(1).split())
admin_classes.discard("pill-")
admin_classes.update({"on", "warn", "err", "pill-placed", "pill-delivered", "pill-cancelled"})
acss_classes = set(re.findall(r"\.([a-zA-Z][\w-]*)", acss_nc))
check("every class used by an admin template has a rule in admin.css", not sorted(c for c in admin_classes if c not in acss_classes), str(sorted(c for c in admin_classes if c not in acss_classes)))

# ---------- admin panel A2: products, restock, hide, categories ----------
def loc(n): return re.search(r"Location: (\S+)", pg(n)["extra"]).group(1) if "Location:" in pg(n)["extra"] else ""

check("A2: every admin POST route refuses guests (302 /login) and customers (403) before doing anything",
      "Location: /login" in pg("admin_guest_post")["extra"] and pg("admin_customer_post")["status"] == 403 and hasnt("admin_customer_post", "Stock updated"))
check("A2: GET on a POST-only route -> 405 + Allow: POST (restock, toggle, category add)",
      all("Allow: POST" in pg(n)["extra"] and pg(n)["status"] == 405 for n in ("ar_get", "at_get", "ac_add_get")))
check("A2: PUT on the add form -> 405 + Allow: GET, POST", "Allow: GET, POST" in pg("ap_new_put")["extra"])

# products list
check("products list: id, name (escaped), category, price, stock, status pill, actions (Edit, restock form, Hide / Show)",
      has("ap_list", "<td>7</td>", "Gadget &lt;b&gt;x&lt;/b&gt;", "<td>Audio</td>", RS + "1,499", '<span class="pill pill-active">Active</span>',
          '<span class="pill pill-hidden">Hidden</span>', 'href="/admin/products/edit?id=7"', 'action="/admin/products/restock"',
          'action="/admin/products/toggle"', '<button class="btn">Hide</button>', '<button class="btn">Show</button>') and hasnt("ap_list", "<b>x</b>"))
check("products list: hidden rows are dimmed and offer 'Show' (active=1); active rows offer 'Hide' (active=0)",
      has("ap_list", '<tr class="dim"><td>8</td>', 'name="active" value="1"', 'name="active" value="0"'))
check("products list: search box, category select (selected kept), 'Show hidden' checkbox (checked kept), count",
      has("ap_list", 'name="q"', '<select name="cat"', '<option value="" selected>All categories</option>', 'name="hidden" value="1" >', "2 products") and
      has("ap_list_filtered", 'value="gad"', '<option value="1" selected>Audio</option>', 'name="hidden" value="1" checked>'))
check("products list: forms send 'return' = this very page (filters kept)", has("ap_list_filtered", 'name="return" value="/admin/products?q=gad&amp;cat=1&amp;hidden=1"'))
check("products list: bad category -> 400, POST -> 405, empty -> friendly row",
      pg("ap_list_badcat")["status"] == 400 and pg("ap_list_post")["status"] == 405 and has("ap_list_empty", "No products match."))
check("products sidebar link is highlighted on the products pages", 'side-link on" href="/admin/products">' in pg("ap_list")["body"] and
      'side-link on" href="/admin/products">' in pg("ap_new_get")["body"])

# form
check("new product form: name, description, price, category (no stock field), POST /admin/products/new",
      has("ap_new_get", 'method="post" action="/admin/products/new"', 'name="name"', 'name="description"', 'name="price"', 'name="category_id"',
          '<option value="1" >Audio</option>', "Stock starts at 0") and hasnt("ap_new_get", 'name="stock"', 'name="product_id"'))
check("new product: valid -> 302 to the list with ?ok=product_added", loc("ap_new_ok") == "/admin/products?ok=product_added")
check("validation: empty name, name over 100, description over 255, price 0 / negative / over 1000000 / text / 3 decimals, bad category -> 400 and the form again",
      all(pg(n)["status"] == 400 for n in ("ap_new_noname", "ap_new_longname", "ap_new_longdesc", "ap_new_price0", "ap_new_priceneg", "ap_new_pricebig",
                                          "ap_new_pricetext", "ap_new_price3dec", "ap_new_catnum", "ap_new_catmissing")) and
      all(has(n, 'class="flash err"', 'method="post" action="/admin/products/new"') for n in ("ap_new_noname", "ap_new_price0", "ap_new_catmissing")))
check("validation: the limits themselves are allowed (name 100, description 255, price 1000000)",
      all(pg(n)["status"] == 302 for n in ("ap_new_name100", "ap_new_desc255", "ap_new_pricemax")))
check("validation messages are specific and escaped; the typed values stay in the form",
      has("ap_new_noname", "Name must be 1 to 100 characters.") and has("ap_new_longdesc", "Description can be at most 255 characters.") and
      has("ap_new_price0", "Price must be more than 0") and has("ap_new_catmissing", "That category does not exist.") and
      has("ap_new_price0", 'value="New &lt;b&gt;Item&lt;/b&gt;"', 'value="0"') and hasnt("ap_new_price0", "<b>Item</b>"))
check("database error while adding -> 500 with the form", pg("ap_new_dberror")["status"] == 500 and has("ap_new_dberror", "Database error"))
check("edit form: filled with the product (escaped), hidden product_id, POST /admin/products/edit, says stock is not changed",
      has("ap_edit_get", 'action="/admin/products/edit"', '<input type="hidden" name="product_id" value="7">', 'value="Gadget &lt;b&gt;x&lt;/b&gt;"',
          "Nice &lt;i&gt;d&lt;/i&gt;", 'value="1499.00"', '<option value="2" selected>Cameras</option>', "Stock is not changed here") and hasnt("ap_edit_get", 'name="stock"'))
check("edit: bad id -> 400, unknown product -> 404 (admin page), valid -> 302 ?ok=product_saved, invalid -> 400 form, vanished -> ?err=no_product",
      pg("ap_edit_badid")["status"] == 400 and pg("ap_edit_404")["status"] == 404 and has("ap_edit_404", "Product not found.") and
      loc("ap_edit_ok") == "/admin/products?ok=product_saved" and pg("ap_edit_invalid")["status"] == 400 and
      loc("ap_edit_gone") == "/admin/products?err=no_product" and loc("ap_edit_badpost_id") == "/admin/products?err=bad_request")

# restock
check("restock: +5, -2 and plain 007 are accepted -> 302 ?ok=restocked", all(loc(n) == "/admin/products?ok=restocked" for n in ("ar_ok_plus", "ar_ok_minus", "ar_ok_plain")))
check("restock: 0, text, empty, decimals, over 100000 -> ?err=bad_delta; bad product id -> ?err=bad_request",
      all(loc(n) == "/admin/products?err=bad_delta" for n in ("ar_zero", "ar_text", "ar_empty", "ar_float", "ar_huge")) and loc("ar_badpid") == "/admin/products?err=bad_request")
check("restock: 0 rows changed and the product exists -> ?err=stock_below; product missing -> ?err=no_product; DB error -> 500",
      loc("ar_below") == "/admin/products?err=stock_below" and loc("ar_noproduct") == "/admin/products?err=no_product" and pg("ar_dberror")["status"] == 500)
check("redirect-back: 'return' to /admin and /admin/products?filters is honoured and the old flash is replaced",
      loc("ar_back_admin") == "/admin?ok=restocked" and loc("ar_back_list") == "/admin/products?q=gad&hidden=1&ok=restocked" and
      loc("ar_back_old_flash") == "/admin/products?ok=restocked")
check("redirect-back is not an open redirect: //evil.com, http://..., /products, /administrator, CRLF all fall back to /admin/products",
      all(loc(n) == "/admin/products?ok=restocked" for n in ("ar_back_evil", "ar_back_http", "ar_back_store", "ar_back_lookalike", "ar_back_crlf")) and
      all("\n" not in pg(n)["extra"].replace(" | ", "") for n in ("ar_back_crlf",)) and "Set-Cookie: x=1" not in pg("ar_back_crlf")["extra"])

# toggle
check("toggle: hide (active=0) -> ?ok=product_hidden, show (active=1) -> ?ok=product_shown, bad values -> ?err=bad_request, missing -> ?err=no_product",
      loc("at_hide") == "/admin/products?ok=product_hidden" and loc("at_show") == "/admin/products?ok=product_shown" and
      loc("at_bad_active") == "/admin/products?err=bad_request" and loc("at_bad_pid") == "/admin/products?err=bad_request" and loc("at_missing") == "/admin/products?err=no_product")

# categories
check("categories list: id, name (escaped), product count, Rename + Delete forms, an add form",
      has("ac_list", "<td>1</td><td>Audio &lt;b&gt;</td><td>12</td>", "<td>2</td><td>Cameras</td><td>0</td>", 'action="/admin/categories/rename"',
          'action="/admin/categories/delete"', 'action="/admin/categories/add"', 'name="category_id" value="1"') and hasnt("ac_list", "Audio <b>") and
      'side-link on" href="/admin/categories">' in pg("ac_list")["body"] and has("ac_list_empty", "No categories yet."))
check("categories: add / rename / delete success -> 302 with the right ?ok= message",
      loc("ac_add_ok") == "/admin/categories?ok=cat_added" and loc("ac_rename_ok") == "/admin/categories?ok=cat_renamed" and
      loc("ac_delete_ok") == "/admin/categories?ok=cat_deleted" and loc("ac_add_50") == "/admin/categories?ok=cat_added")
check("categories: error 1062 (duplicate name) -> 'already exists' for add and rename; error 1451 -> 'has products'; 0 rows -> 'not found'; other DB error -> 500",
      loc("ac_add_dup") == "/admin/categories?err=cat_exists" and loc("ac_rename_dup") == "/admin/categories?err=cat_exists" and
      loc("ac_delete_fk") == "/admin/categories?err=cat_has_products" and loc("ac_delete_missing") == "/admin/categories?err=cat_missing" and
      pg("ac_add_dberror")["status"] == 500)
check("categories: empty / over 50 characters -> ?err=bad_name, bad id -> ?err=bad_request",
      loc("ac_add_empty") == "/admin/categories?err=bad_name" and loc("ac_add_long") == "/admin/categories?err=bad_name" and
      loc("ac_rename_noname") == "/admin/categories?err=bad_name" and loc("ac_rename_badid") == "/admin/categories?err=bad_request" and loc("ac_delete_badid") == "/admin/categories?err=bad_request")
check("a successful category change clears the navbar cache at once (old name before, new name right after)",
      "CACHE_DEMO Audio" in raw_out and "CACHE_CLEARED Renamed" in raw_out)

# flash messages come from fixed texts keyed by a code in the URL
check("flash: ?ok=restocked is a green box and ?err=stock_below a red box on the admin pages (fixed texts)",
      has("ap_flash_ok", '<div class="flash ok">Stock updated.</div>') and has("ap_flash_err", '<div class="flash err">Stock cannot go below 0.</div>') and
      has("ap_flash_cat", "already exists") and has("ap_flash_fk", "Move its products to another category first"))
check("flash: unknown codes and HTML in the code show nothing (the URL can never inject text)",
      hasnt("ap_flash_junk", 'class="flash') and hasnt("ap_flash_junk", "<script"))
check("flash is shown above the page content", pg("ap_flash_ok")["body"].index("flash ok") < pg("ap_flash_ok")["body"].index("<h1>Products</h1>"))

# dashboard restock form
check("dashboard low-stock list has a restock form per product (product_id + delta, back to /admin)",
      has("admin", 'action="/admin/products/restock"', '<input type="hidden" name="product_id" value="31">', '<input type="hidden" name="return" value="/admin">',
          'name="delta"', "Smart LED &lt;b&gt;Bulb&lt;/b&gt;") and adm.count('action="/admin/products/restock"') == 2)

# store side: hidden products
src_pages = src["pages.cpp"]; src_db = src["db.cpp"]
check("store: lists / search / home budget picks / product page / cart-add all filter on is_active",
      src_pages.count("p.is_active = TRUE") >= 3 and "WHERE product_id = ? AND is_active = TRUE" in src_pages)
check("store: checkout locks is_active with the other product columns and rolls back with '<name> is no longer available.'",
      "p.stock, p.is_active" in src_db and '" is no longer available."' in src_db and 'row[5] == "0"' in src_db)
check("store: restock is ONE atomic statement (stock = stock + ... with a >= 0 guard), never SET stock = <number>",
      "stock = stock + CAST(? AS SIGNED)" in src_pages and "stock + CAST(? AS SIGNED) >= 0" in src_pages and
      not re.search(r"SET stock = \?", src_pages) and not re.search(r"SET stock = [0-9]", src_pages))
check("admin edit never touches stock (UPDATE products SET name, description, price, category_id only)",
      'UPDATE products SET name = ?, description = ?, price = ?, category_id = ? WHERE product_id = ?' in src_pages)
check("products are never deleted by the admin code", "DELETE FROM products" not in src_pages)
check("cart page: a hidden product in the cart shows 'No longer available'", has("cart_hidden", "No longer available"))

# navbar labels
check("store navbar labels: 'Your Orders' and 'Your Sessions' (no 'Returns' or 'Login' label)",
      has("cart", '<a class="acct" href="/orders"><small>Your</small><strong>Orders</strong></a>', '<a class="acct" href="/sessions"><small>Your</small><strong>Sessions</strong></a>') and
      hasnt("cart", "Returns", "<small>Login</small>"))

# admin.css additions
check("admin.css: buttons, filters, form, flash and pill rules exist", all(x in acss for x in (".btn-primary", ".btn-danger", ".filters", ".form", ".flash.ok", ".flash.err", ".pill-active", ".pill-hidden", ".dim", ".restock")))

# ---------- A3: orders ----------
src_pages = src["pages.cpp"]; src_db = src["db.cpp"]
def forms(n): return re.findall(r'<form class="inline" method="post" action="/admin/order/status">.*?</form>', pg(n)["body"], re.S)
def hidden_tos(n): return sorted(re.findall(r'name="to" value="(\w+)"', pg(n)["body"]))

check("orders list: #id link, customer name + email (escaped), date, items, total, order status pill, payment status pill",
      has("ao_list", '<a href="/admin/order?id=14">#14</a>', "Aarav &lt;b&gt;Sharma&lt;/b&gt;<br><small class=\"muted\">aarav@x.com</small>", "2026-10-13 19:29:39",
          "<td>3</td>", RS + "33,892", '<span class="pill pill-placed">PLACED</span>', '<span class="pill pill-pending">PENDING</span>',
          '<span class="pill pill-delivered">DELIVERED</span>', '<span class="pill pill-paid">PAID</span>', '<span class="pill pill-cancelled">CANCELLED</span>',
          '<span class="pill pill-shipped">SHIPPED</span>') and hasnt("ao_list", "<b>Sharma</b>") and 'side-link on" href="/admin/orders">' in pg("ao_list")["body"])
check("orders list: columns #, Customer, Date, Items, Total, Status, Payment; count text",
      has("ao_list", "<th>#</th><th>Customer</th><th>Date</th><th>Items</th><th>Total</th><th>Status</th><th>Payment</th>", "4 orders on this page"))
check("orders list: no payment row shows '-'", has("ao_list", '<span class="pill pill--">-</span>') or has("ao_list", ">-</span>"))
check("orders filter: status select (All + PLACED, SHIPPED, DELIVERED, CANCELLED; chosen one selected) and the search box keeps its text",
      has("ao_list", '<option value="" selected>All statuses</option>', '<option value="PLACED" >PLACED</option>', '<option value="CANCELLED" >CANCELLED</option>',
          'name="q"', "Order number or customer email") and
      has("ao_list_filtered", '<option value="SHIPPED" selected>SHIPPED</option>', 'value="a%b%20c"'))
check("orders filter: unknown status / page 0 / page abc -> 400, POST -> 405", all(pg(n)["status"] == 400 for n in ("ao_list_badstatus", "ao_list_badpage", "ao_list_badpage2")) and pg("ao_list_post")["status"] == 405)
check("orders list: empty result says so; no pager when everything fits", has("ao_empty", "No orders match.") and hasnt("ao_empty", 'class="pager"') and hasnt("ao_list", 'class="pager"'))
check("pagination: 20 rows per page (21st row only tells there is a next page); 'Older' link keeps status and q (encoded)",
      pg("ao_page1")["body"].count('href="/admin/order?id=') == 20 and has("ao_page1", 'href="/admin/orders?status=PLACED&amp;q=c%40x.com&amp;page=2"', "Older") and
      hasnt("ao_page1", "Newer") and has("ao_page1", "Page 1"))
check("pagination: page 2 has a 'Newer' link to page 1 (and Older again when more rows exist)",
      has("ao_page2", 'href="/admin/orders?page=1"', "Newer", 'href="/admin/orders?page=3"', "Page 2"))
check("source: LIMIT 21 OFFSET (page-1)*20 built from ints (no user text in LIMIT), filter values only as ? args",
      "LIMIT \" + to_string(ORDERS_PER_PAGE + 1) + \" OFFSET \"" in src_pages and "o.status = ?" in src_pages and "u.email LIKE ? ESCAPE '!'" in src_pages and "const int ORDERS_PER_PAGE = 20;" in src_pages)

check("order detail: customer (escaped), items with the price at purchase, total, status pill, payment pill, placed date",
      has("ao_detail_PLACED", "<h1>Order #7</h1>", "Ann &lt;b&gt;", "ann@x.com", "Gadget &lt;b&gt;x&lt;/b&gt;", "<td>2</td>", RS + "499<", RS + "998<",
          "Price at purchase", '<span class="pill pill-placed">PLACED</span>', 'COD <span class="pill pill-pending">PENDING</span>', "2026-10-24 10:00:00") and
      hasnt("ao_detail_PLACED", "Ann <b>") and 'side-link on" href="/admin/orders">' in pg("ao_detail_PLACED")["body"])
check("order detail PLACED: buttons Ship and Cancel only (no Deliver), each a POST form with order_id, to, from",
      hidden_tos("ao_detail_PLACED") == ["CANCELLED", "SHIPPED"] and len(forms("ao_detail_PLACED")) == 2 and
      has("ao_detail_PLACED", 'name="order_id" value="7"', 'name="from" value="PLACED"', "Mark as shipped", "Cancel order (items go back to stock)") and hasnt("ao_detail_PLACED", "Mark as delivered"))
check("order detail SHIPPED: buttons Deliver and Cancel only (no Ship)", hidden_tos("ao_detail_SHIPPED") == ["CANCELLED", "DELIVERED"] and
      has("ao_detail_SHIPPED", "Mark as delivered (payment becomes PAID)", 'name="from" value="SHIPPED"') and hasnt("ao_detail_SHIPPED", "Mark as shipped"))
check("order detail DELIVERED / CANCELLED: no buttons at all, 'final' text", all(not forms(n) and has(n, "This order is final") for n in ("ao_detail_DELIVERED", "ao_detail_CANCELLED")) and
      has("ao_detail_DELIVERED", '<span class="pill pill-paid">PAID</span>, paid 2026-10-25 09:00:00') and has("ao_detail_CANCELLED", '<span class="pill pill-cancelled">CANCELLED</span>'))
check("order detail: 404 admin page, bad id 400, POST 405, guest 302 /login, customer 403",
      pg("ao_detail_404")["status"] == 404 and has("ao_detail_404", "Order not found.", "admin.css") and pg("ao_detail_badid")["status"] == 400 and
      pg("ao_detail_post")["status"] == 405 and "Location: /login" in pg("ao_detail_guest")["extra"] and pg("ao_detail_customer")["status"] == 403 and
      hasnt("ao_detail_customer", "ann@x.com"))

def loc(n): return re.search(r"Location: (\S+)", pg(n)["extra"]).group(1) if "Location:" in pg(n)["extra"] else ""
check("status change success: 302 back to the order with a green message code (ship / deliver / cancel)",
      loc("as_ship") == "/admin/order?id=7&ok=order_shipped" and loc("as_deliver") == "/admin/order?id=7&ok=order_delivered" and loc("as_cancel") == "/admin/order?id=7&ok=order_cancelled")
check("status change: 400 'Not allowed' is an admin page with the reason; 404 / 409 / 503 show their message; other errors 500",
      pg("as_notallowed")["status"] == 400 and has("as_notallowed", "Not allowed: an order cannot go from DELIVERED to SHIPPED.", "admin.css") and
      has("as_404", "Order not found") and has("as_409", "changed by someone else") and has("as_503", "busy right now") and pg("as_500")["status"] == 500)
check("status change: bad order id, target PLACED / PENDING, junk 'from' -> redirect with ?err=bad_request; GET -> 405",
      all(loc(n) == "/admin/orders?err=bad_request" for n in ("as_bad_to", "as_bad_to2", "as_bad_id", "as_bad_from")) and "Allow: POST" in pg("as_get")["extra"])
check("status change: customers (403), guests (302) and bad input NEVER reach order_change()", "ORDER_CALLS_BAD 0" in raw_out and pg("as_customer")["status"] == 403 and
      "Location: /login" in pg("as_guest")["extra"])
check("flash texts for the three changes exist and the log line is [ADMIN] user N order N FROM->TO",
      all(x in src_pages for x in ("Order marked as shipped.", "The payment is now PAID.", "Its items were added back to stock.")) and
      'log_tag("ADMIN", "user " + to_string(req.user_id) + " order " + id + " " + r.from + "->" + to)' in src_pages)
check("admin buttons come from the same transition rule as the server (order_transition_ok) and the dashboard links to the order",
      "order_transition_ok(o[1], n.to)" in src_pages and has("admin", 'href="/admin/order?id=14"'))

# customer side
check("customer order page: 'Cancel order' form (POST /order/cancel, order_id) only while PLACED",
      has("oc_PLACED", 'method="post" action="/order/cancel"', 'name="order_id" value="7"', "Cancel order") and
      all(hasnt(n, "/order/cancel", "Cancel order") for n in ("oc_SHIPPED", "oc_DELIVERED", "oc_CANCELLED")))
check("customer order page: green message after cancelling", has("oc_flash", '<div class="flash ok">Your order was cancelled. Its items went back to stock.</div>') and hasnt("oc_PLACED", "was cancelled"))
check("customer cancel: success -> 302 /order?id=7&ok=cancelled and order_change got (order 7, CANCELLED, session user 5, NOT admin, no expected status)",
      loc("oc_cancel_ok") == "/order?id=7&ok=cancelled" and "CUSTOMER_CALL 7|CANCELLED|5|customer|" in raw_out)
check("customer cancel: 400 -> 'can no longer be cancelled', 404 -> not found, 503 -> busy, 500 -> database error",
      has("oc_cancel_400", "<h1>This order can no longer be cancelled.</h1>") and pg("oc_cancel_404")["status"] == 404 and has("oc_cancel_404", "Order not found") and
      pg("oc_cancel_503")["status"] == 503 and pg("oc_cancel_500")["status"] == 500)
check("customer cancel: guest -> /login, GET -> 405 (Allow: POST), bad id -> 400, and none of those reach order_change()",
      "Location: /login" in pg("oc_cancel_guest")["extra"] and "Allow: POST" in pg("oc_cancel_get")["extra"] and pg("oc_cancel_badid")["status"] == 400 and "CUSTOMER_BAD_CALLS 0" in raw_out)
check("no admin link or admin.css on the customer order pages", all('href="/admin' not in pg(n)["body"] and "admin.css" not in pg(n)["body"] for n in ("oc_PLACED", "oc_flash")))

# source safeguards
check("db.cpp: cancel is one transaction: order FOR UPDATE, items in product_id order, stock = stock + qty, status UPDATE ... AND status = old, payment updated, same retry + autocommit reset",
      "SELECT status, user_id FROM orders WHERE order_id = ? FOR UPDATE" in src_db and "ORDER BY product_id" in src_db and "UPDATE products SET stock = stock + ? WHERE product_id = ?" in src_db and
      "UPDATE orders SET status = ? WHERE order_id = ? AND status = ?" in src_db and "UPDATE payments SET status = 'PAID', paid_at = NOW() WHERE order_id = ?" in src_db and
      "UPDATE payments SET status = 'CANCELLED' WHERE order_id = ?" in src_db and "mysql_rollback(m);" in src_db)
check("db.cpp: only allowed transitions (PLACED->SHIPPED/CANCELLED, SHIPPED->DELIVERED/CANCELLED); statuses used are all in the CHECK list",
      'from == "PLACED" && to == "SHIPPED"' in src_db and 'from == "SHIPPED" && to == "DELIVERED"' in src_db and 'from == "PLACED" && to == "CANCELLED"' in src_db and
      'from == "SHIPPED" && to == "CANCELLED"' in src_db and all(s in read(r"sql\ecommerce_db.sql") for s in ("'SHIPPED'", "'DELIVERED'", "'CANCELLED'", "'PAID'")))

# ---------- A4: users, sessions, logs, audit ----------
A4_STATUS = {"au_list": 200, "au_search": 200, "au_badpage": 400, "au_post": 405, "au_page1": 200, "au_page2": 200, "au_empty": 200,
             "au_role_ok": 302, "au_role_own_role": 302, "au_role_last_admin": 302, "au_role_same_role": 302, "au_role_not_admin": 302, "au_role_no_user": 302,
             "au_role_busy": 503, "au_role_500": 500, "au_role_bad_role": 302, "au_role_bad_id": 302, "au_role_get": 405, "au_role_customer": 403, "au_role_guest": 302,
             "au_la_ok": 302, "au_la_none": 302, "au_la_own": 302, "au_la_bad": 302, "au_la_get": 405, "au_la_dberror": 500,
             "sx_list": 200, "sx_all": 200, "sx_badshow": 400, "sx_badpage": 400, "sx_post": 405, "sx_page1": 200, "sx_empty": 200,
             "sx_end_ok": 302, "sx_end_full": 302, "sx_end_own": 302, "sx_end_gone": 302, "sx_end_ambiguous": 302, "sx_end_race": 302, "sx_end_dberror": 500,
             "sx_end_get": 405, "sx_end_customer": 403, "sx_end_guest": 302, "sx_customer_page": 200,
             "lg_list": 200, "lg_filtered": 200, "lg_refresh": 200, "lg_badworker": 400, "lg_badcls": 400, "lg_post": 405, "lg_empty": 200, "lg_guest": 302, "lg_customer": 403}
bad_status = {k: pg(k)["status"] for k, v in A4_STATUS.items() if pg(k)["status"] != v}
check("A4: status codes of all %d new cases are as designed" % len(A4_STATUS), not bad_status, str(bad_status))
check("A4: no template was asked for a missing value", all(pg(k)["why"] == "" for k in A4_STATUS))

def audits_before(name):
    """AUDIT lines printed right before the '=== name' block (what that one request wrote)"""
    head = raw_out[:raw_out.index("=== %s\n" % name)]
    tail_lines = head.split("=== END")[-1].strip().splitlines()
    return [l for l in tail_lines if l.startswith("AUDIT ")]

# users
check("users list: id, name + email (escaped), role pill, joined, orders count, active sessions count",
      has("au_list", "<td>6</td><td>Bob</td><td>bob@x.com</td>", '<span class="pill pill-admin">ADMIN</span>', '<span class="pill pill-customer">CUSTOMER</span>',
          "2026-02-01 10:00:00", "<td>12</td><td>2</td>", "Ann &lt;b&gt;") and hasnt("au_list", "Ann <b>") and 'side-link on" href="/admin/users">' in pg("au_list")["body"])
check("users list: columns", has("au_list", "<th>#</th><th>Name</th><th>Email</th><th>Role</th><th>Joined</th><th>Orders</th><th>Active sessions</th><th>Actions</th>", "4 users on this page"))
my_row = pg("au_list")["body"].split("<tr><td>5</td>")[1].split("</tr>")[0]
check("users list: my own row (id 5) says '(you)' and has NO role or logout buttons",
      "(you)" in my_row and "users/role" not in my_row and "logout-all" not in my_row and "<form" not in my_row)
check("users list: admins get 'Make customer' (role=CUSTOMER), customers get 'Make admin' (role=ADMIN), each a POST form with user_id",
      has("au_list", 'name="user_id" value="8"><input type="hidden" name="role" value="CUSTOMER"', 'name="user_id" value="6"><input type="hidden" name="role" value="ADMIN"',
          "Make customer", "Make admin", 'method="post" action="/admin/users/role"'))
check("users list: 'Log out everywhere' only for users with active sessions (user 6 only)",
      pg("au_list")["body"].count('action="/admin/users/logout-all"') == 1 and has("au_list", 'action="/admin/users/logout-all"><input type="hidden" name="user_id" value="6">', "Log out everywhere"))
check("users search: box keeps the text; bad page -> 400; POST -> 405; empty -> friendly row; forms return to this very page",
      has("au_search", 'value="a%b_c"') and pg("au_badpage")["status"] == 400 and pg("au_post")["status"] == 405 and has("au_empty", "No users match.") and
      has("au_search", 'name="return" value="/admin/users?q=a%b_c"'))
check("users pager: 20 per page with Older / Newer links keeping q", pg("au_page1")["body"].count("<td>1") >= 0 and has("au_page1", 'href="/admin/users?q=u&amp;page=2"', "Older") and
      has("au_page2", 'href="/admin/users?page=1"', "Newer", 'href="/admin/users?page=3"') and pg("au_page1")["body"].count('<td class="actions">') == 20)
check("users: SQL counts orders and ACTIVE (not expired) sessions in subqueries; LIMIT/OFFSET are ints",
      "(SELECT COUNT(*) FROM orders o WHERE o.user_id = u.user_id)" in src_pages and "s.is_active = 1 AND s.expires_at > NOW()" in src_pages and "u.name LIKE ? ESCAPE '!' OR u.email LIKE ? ESCAPE '!'" in src_pages)

# role change
check("role change: success -> 302 /admin/users?ok=role_changed; the call is (admin = session user 5, user 6, ADMIN)",
      loc("au_role_ok") == "/admin/users?ok=role_changed" and "ROLE_CALL 5|6|ADMIN" in raw_out)
check("role change refusals come back as red messages: own_role, last_admin, same_role, not_admin, no_user",
      all(loc("au_role_" + c) == "/admin/users?err=" + c for c in ("own_role", "last_admin", "same_role", "not_admin", "no_user")))
check("role change: busy -> 503, database error -> 500, bad role / bad id -> ?err=bad_request, GET -> 405",
      pg("au_role_busy")["status"] == 503 and pg("au_role_500")["status"] == 500 and loc("au_role_bad_role") == "/admin/users?err=bad_request" and
      loc("au_role_bad_id") == "/admin/users?err=bad_request" and "Allow: POST" in pg("au_role_get")["extra"])
check("role change: customers (403), guests (302) and bad input NEVER reach role_change()", "ROLE_BAD_CALLS 0" in raw_out and pg("au_role_customer")["status"] == 403)
check("flash texts exist for role_changed, own_role, last_admin", all(x in src_pages for x in ("Role changed.", "You cannot change your own role.", "The last admin cannot be demoted")))
check("role change writes its audit row inside the transaction (db.cpp), not through the page", not audits_before("au_role_ok") and "'USER_ROLE'" in src_db)

# logout-all
check("logout everywhere: ok -> ?ok=sessions_ended, nothing to end -> ?err=no_sessions, own id -> ?err=own_user, bad id -> bad_request, DB error 500, GET 405",
      loc("au_la_ok") == "/admin/users?ok=sessions_ended" and loc("au_la_none") == "/admin/users?err=no_sessions" and loc("au_la_own") == "/admin/users?err=own_user" and
      loc("au_la_bad") == "/admin/users?err=bad_request" and pg("au_la_dberror")["status"] == 500 and "Allow: POST" in pg("au_la_get")["extra"])
check("logout everywhere: audit row only on success ('user 6, 3 sessions ended')",
      audits_before("au_la_ok") == ["AUDIT 5|USER_LOGOUT_ALL|user 6, 3 sessions ended"] and not audits_before("au_la_none") and not audits_before("au_la_own"), str(audits_before("au_la_ok")))
check("source: end uses end_reason 'FORCED' with logout_time = NOW() and only touches active sessions",
      src_pages.count("SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED'") == 2)

# sessions
sx = pg("sx_all")["body"]
check("sessions list: user (escaped) + email, only the FIRST 8 characters of each session id, login, last activity, expires",
      has("sx_all", "<code>aaaaaaaa</code>", "<code>bbbbbbbb</code>", "Ann &lt;b&gt;", "ann@x.com", "2026-10-24 10:00:00", "2026-10-24 10:35:00") and
      "a" * 9 not in sx and "b" * 9 not in sx and "c" * 9 not in sx and 'side-link on" href="/admin/sessions">' in sx)
check("sessions list: statuses Active / Logged out / Expired / Ended by admin as pills",
      has("sx_all", '<span class="pill pill-active">Active</span>', '<span class="pill pill-loggedout">Logged out</span>', '<span class="pill pill-expired">Expired</span>',
          '<span class="pill pill-endedbyadmin">Ended by admin</span>'))
check("sessions list: 'End session' only for ACTIVE sessions that are not mine; mine says '(you)'; the form carries just 8 characters",
      sx.count('action="/admin/sessions/end"') == 1 and has("sx_all", '<input type="hidden" name="session_id" value="bbbbbbbb">', "End session") and
      bool(re.search(r"<code>aaaaaaaa</code>.*?\(you\)", sx, re.S)) and not re.search(r'name="session_id" value="[0-9a-f]{9,}"', sx))
check("sessions filter: Active (default) / All tabs, the chosen one highlighted; bad filter / page -> 400; POST -> 405",
      has("sx_list", '<a class="tab on" href="/admin/sessions?show=active">Active</a>', '<a class="tab" href="/admin/sessions?show=all">All</a>') and
      has("sx_all", '<a class="tab on" href="/admin/sessions?show=all">All</a>') and pg("sx_badshow")["status"] == 400 and pg("sx_badpage")["status"] == 400 and pg("sx_post")["status"] == 405)
check("sessions: pager keeps the filter (20 per page); empty text", has("sx_page1", 'href="/admin/sessions?show=all&amp;page=2"', "Older") and pg("sx_page1")["body"].count("<code>ffffffff</code>") == 20 and
      has("sx_empty", "No sessions to show."))
check("source: the Active filter is SQL (is_active = 1 AND expires_at > NOW()), not just a label",
      'sql += " WHERE s.is_active = 1 AND s.expires_at > NOW()";' in src_pages)
check("end session: ok (8 chars or full id) -> ?ok=session_ended; my own session -> ?err=own_session; unknown -> session_gone; ambiguous 8 chars -> bad_request; lost the race -> session_gone",
      loc("sx_end_ok") == "/admin/sessions?ok=session_ended" and loc("sx_end_full") == "/admin/sessions?ok=session_ended" and loc("sx_end_own") == "/admin/sessions?err=own_session" and
      loc("sx_end_gone") == "/admin/sessions?err=session_gone" and loc("sx_end_ambiguous") == "/admin/sessions?err=bad_request" and loc("sx_end_race") == "/admin/sessions?err=session_gone")
check("end session: not hex / wrong length / SQL text -> ?err=bad_request (nothing queried); DB error 500; GET 405; customers 403; guests /login",
      all(loc(n) == "/admin/sessions?err=bad_request" for n in pages if n.startswith("sx_end_bad_")) and len([n for n in pages if n.startswith("sx_end_bad_")]) == 6 and
      pg("sx_end_dberror")["status"] == 500 and "Allow: POST" in pg("sx_end_get")["extra"] and pg("sx_end_customer")["status"] == 403 and "Location: /login" in pg("sx_end_guest")["extra"])
check("end session: audit row 'SESSION_END session bbbbbbbb' on success only",
      audits_before("sx_end_ok") == ["AUDIT 5|SESSION_END|session bbbbbbbb"] and all(not audits_before(n) for n in ("sx_end_own", "sx_end_gone", "sx_end_race", "sx_end_ambiguous", "sx_end_bad_8z")))
check("customer's own /sessions page shows 'Ended by admin' (red pill) for a forced session",
      has("sx_customer_page", '<span class="pill pill-endedbyadmin">Ended by admin</span>', '<span class="pill pill-active">Active</span>'))

# logs
check("logs: worker table (requests, avg latency, CSS bar width relative to the busiest worker)",
      has("lg_list", "Requests per worker (last 5 minutes)", "<td>#2</td><td>50</td><td>1.50</td>", '<span style="width:100%"></span>', "<td>#3</td><td>25</td><td>3.13</td>",
          '<span style="width:50%"></span>', '<span style="width:0%"></span>') and 'side-link on" href="/admin/logs">' in pg("lg_list")["body"])
check("logs: recent requests (time, method, path escaped, status coloured by class, latency, queue wait, worker, user escaped)",
      has("lg_list", "<td>2026-10-24 10:00:01</td><td>GET</td><td>/products?q=&lt;b&gt;</td>", '<span class="code s2">200</span>', '<span class="code s5">503</span>',
          '<span class="code s4">404</span>', '<span class="code s3">302</span>', "<td>1.250</td><td>0.100</td><td>#2</td><td>Ann &lt;b&gt; (#5)</td>", "4 of the last 100 requests") and hasnt("lg_list", "<b>"))
check("logs: filters worker, status class and path text keep their values; auto refresh checkbox",
      has("lg_filtered", 'name="worker" value="2"', '<option value="5xx" selected>5xx</option>', 'value="%25a_b"') and has("lg_list", '<option value="" selected>All status classes</option>', "Refresh every 3 s"))
check("logs: ?refresh=1 adds a 3 second meta refresh in <head> and keeps the box ticked; without it there is no meta refresh",
      has("lg_refresh", '<meta http-equiv="refresh" content="3">', 'name="refresh" value="1" checked>') and hasnt("lg_list", 'http-equiv="refresh"') and hasnt("lg_list", 'value="1" checked'))
check("logs: bad worker / bad status class -> 400, POST -> 405, empty database -> friendly rows, guests /login, customers 403",
      pg("lg_badworker")["status"] == 400 and pg("lg_badcls")["status"] == 400 and pg("lg_post")["status"] == 405 and
      has("lg_empty", "No requests in the last 5 minutes.", "No log rows match.", "No admin changes yet.") and "Location: /login" in pg("lg_guest")["extra"] and
      pg("lg_customer")["status"] == 403 and hasnt("lg_customer", "request_logs"))
check("logs: Admin activity table (last 50: time, admin, action, details escaped)",
      has("lg_list", "Admin activity", "<td>Boss &lt;i&gt;</td><td><code>ORDER_STATUS</code></td><td>order 7 PLACED-&gt;SHIPPED</td>", "product 3 by +5 &lt;b&gt;") and
      "ORDER BY a.action_id DESC LIMIT 50" in src_pages and "ORDER BY l.log_id DESC LIMIT 100" in src_pages and "GROUP BY worker_id" in src_pages and "INTERVAL 5 MINUTE" in src_pages)
check("logs: filters are SQL args (worker, path) and fixed numbers (status class)",
      'sql += " AND l.worker_id = ?";' in src_pages and "l.path LIKE ? ESCAPE '!'" in src_pages and
      'sql += " AND l.status BETWEEN " + to_string(lo) + " AND " + to_string(lo + 99);' in src_pages)

# audit
check("audit: every admin change has a row: product add / edit / restock / hide / show, category add / rename / delete (right after the change)",
      audits_before("ap_new_ok") == ["AUDIT 5|PRODUCT_ADD|product 'New <b>Item</b>' price 499.50 category 1"] and
      audits_before("ap_edit_ok") and audits_before("ap_edit_ok")[0].startswith("AUDIT 5|PRODUCT_EDIT|product 7 ") and
      audits_before("ar_ok_plus") == ["AUDIT 5|PRODUCT_RESTOCK|product 7 by 5"] and audits_before("ar_ok_minus") == ["AUDIT 5|PRODUCT_RESTOCK|product 7 by -2"] and
      audits_before("at_hide") == ["AUDIT 5|PRODUCT_HIDE|product 7"] and audits_before("at_show") == ["AUDIT 5|PRODUCT_SHOW|product 7"] and
      audits_before("ac_add_ok") == ["AUDIT 5|CATEGORY_ADD|category 'Garden'"] and audits_before("ac_rename_ok") == ["AUDIT 5|CATEGORY_RENAME|category 2 renamed to 'Photo'"] and
      audits_before("ac_delete_ok") == ["AUDIT 5|CATEGORY_DELETE|category 2"], str([audits_before(n) for n in ("ap_new_ok", "ar_ok_plus", "at_hide", "ac_add_ok")]))
check("audit: a refused or failed change writes NO row (validation errors, 0 rows changed, database errors)",
      all(not audits_before(n) for n in ("ap_new_noname", "ap_new_price0", "ap_new_dberror", "ap_edit_gone", "ar_zero", "ar_below", "ar_noproduct", "ar_dberror",
                                          "at_bad_active", "at_missing", "ac_add_dup", "ac_delete_fk", "ac_add_empty", "ac_delete_missing", "ac_add_dberror")))
check("audit: source has an action name for every kind of change (pages.cpp + db.cpp)",
      all(a in src_pages for a in ("PRODUCT_ADD", "PRODUCT_EDIT", "PRODUCT_RESTOCK", "PRODUCT_HIDE", "PRODUCT_SHOW", "CATEGORY_ADD", "CATEGORY_RENAME", "CATEGORY_DELETE", "SESSION_END", "USER_LOGOUT_ALL")) and
      "'ORDER_STATUS'" in src_db and "'USER_ROLE'" in src_db and "INSERT INTO admin_actions (admin_id, action, details) VALUES (?, ?, ?)" in src_db)
check("audit: order status audit is written inside the order transaction (admins only), role audit inside the role transaction",
      "if (as_admin && !exec_one(m, \"INSERT INTO admin_actions" in src_db and src_db.index("INSERT INTO admin_actions (admin_id, action, details) VALUES (?, 'ORDER_STATUS', ?)") < src_db.index("static int order_attempt"))
check("no admin link, admin.css or session/audit data on customer pages", all('href="/admin' not in pg(n)["body"] and "admin.css" not in pg(n)["body"] for n in ("sx_customer_page",)))

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
