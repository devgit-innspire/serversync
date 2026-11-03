# test_checkout.py - tests the checkout TRANSACTION (do_checkout in src/db.cpp) on a throwaway MySQL.
# Never reads or writes db.conf and never touches your real database (see throwaway_mysql.py).
#     python tests\test_checkout.py
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as db

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def read(p):
    with open(os.path.join(ROOT, p), encoding="utf-8") as f:
        return f.read()

def sql(q):
    return db.run(q, "ecommerce_db")

work = tempfile.mkdtemp(prefix="serversync_checkout_")

def build(name, delay=0, fail=0):
    """compile checkout_check.cpp against a copy of include/ with the two test settings changed"""
    inc = os.path.join(work, "include_" + name)
    shutil.copytree(os.path.join(ROOT, "include"), inc)
    path = os.path.join(inc, "common.h")
    text = open(path, encoding="utf-8").read()
    text = text.replace("const int CHECKOUT_DELAY_MS = 0;", "const int CHECKOUT_DELAY_MS = %d;" % delay)
    text = text.replace("const int TEST_FAIL_CHECKOUT = 0;", "const int TEST_FAIL_CHECKOUT = %d;" % fail)
    assert ("= %d;" % delay) in text and ("TEST_FAIL_CHECKOUT = %d;" % fail) in text
    open(path, "w", encoding="utf-8").write(text)
    exe = os.path.join(work, name + ".exe")
    r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "checkout_check.cpp"),
                        "-I" + inc, "-I" + os.path.join(db.BASE, "include"), "-L" + os.path.join(db.BASE, "lib"),
                        "-lmysql", "-lws2_32", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[:3000])
        raise SystemExit("build failed: " + name)
    shutil.copy(os.path.join(ROOT, "libmysql.dll"), work)
    return exe

def run_checkout(exe, users):
    r = subprocess.run([exe] + [str(u) for u in users], capture_output=True, text=True, cwd=work)
    results = []
    for m in re.finditer(r"RESULT user=(\d+) status=(\d+) order=(\d+) total=(\S*) msg=(.*)", r.stdout):
        results.append({"user": int(m[1]), "status": int(m[2]), "order": int(m[3]), "total": m[4], "msg": m[5]})
    autocommit = re.findall(r"AUTOCOMMIT conn=\d value=(\d)", r.stdout)
    return results, autocommit, r.stdout + r.stderr

def fresh():
    db.run(read(r"sql\ecommerce_db.sql"))
    sql("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','secret1'),('b','b@x.com','secret1'),"
        "('c','c@x.com','secret1')")

def cart(user, items):
    """ACTIVE cart for `user` with items [(product_id, qty)]"""
    sql("INSERT INTO cart (user_id) VALUES (%d)" % user)
    cid = sql("SELECT cart_id FROM cart WHERE user_id=%d AND status='ACTIVE'" % user)
    for pid, qty in items:
        sql("INSERT INTO cart_items (cart_id, product_id, quantity) VALUES (%s,%d,%d)" % (cid, pid, qty))
    return cid

def counts():
    return [sql("SELECT COUNT(*) FROM " + t) for t in ("orders", "order_items", "payments")]

try:
    db.start()
    normal = build("normal")
    slow = build("slow", delay=400)
    failing = build("failing", fail=1)
    check("test settings default to 0 in common.h",
          "CHECKOUT_DELAY_MS = 0;" in read(r"include\common.h") and "TEST_FAIL_CHECKOUT = 0;" in read(r"include\common.h"))

    # --- 1. happy path: everything in one transaction ---
    fresh()
    sql("UPDATE products SET stock = 40 WHERE product_id = 1; UPDATE products SET stock = 15 WHERE product_id = 2")
    cid = cart(1, [(1, 2), (2, 1)])                  # 2 x 499.00 + 1 x 3999.00 = 4997.00
    res, ac, out = run_checkout(normal, [1])
    check("happy path: status 200 with an order id", res and res[0]["status"] == 200 and res[0]["order"] > 0, str(res))
    check("total comes from DB prices: 4997.00", res and res[0]["total"] == "4997.00", str(res))
    o = sql("SELECT user_id, cart_id, status, total FROM orders").split("\t")
    check("orders row: PLACED, right user/cart/total", o == ["1", cid, "PLACED", "4997.00"], str(o))
    items = sql("SELECT product_id, quantity, price FROM order_items ORDER BY product_id").replace("\n", "|")
    check("order_items keep the price at purchase time", items == "1\t2\t499.00|2\t1\t3999.00", items)
    st = sql("SELECT (SELECT stock FROM products WHERE product_id=1), (SELECT stock FROM products WHERE product_id=2)")
    check("stock reduced (40-2, 15-1)", st == "38\t14", st)
    p = sql("SELECT method, status, amount, IFNULL(paid_at,'NULL') FROM payments")
    check("payment: COD, PENDING, paid_at NULL", p == "COD\tPENDING\t4997.00\tNULL", p)
    check("cart is CHECKED_OUT", sql("SELECT status FROM cart WHERE cart_id=%s" % cid) == "CHECKED_OUT")
    check("log shows COMMIT", "order %d total 4997.00 COMMIT" % res[0]["order"] in out, out[-200:])
    check("autocommit is back ON on every pooled connection", ac == ["1"] * 4, str(ac))
    # price change later must not change the old order
    sql("UPDATE products SET price = 1.00 WHERE product_id = 1")
    check("old order unaffected by a later price change", sql("SELECT total FROM orders") == "4997.00" and
          sql("SELECT price FROM order_items WHERE product_id=1") == "499.00")
    # the user can shop again: a new ACTIVE cart is allowed after checkout
    n = sql("INSERT INTO cart (user_id) VALUES (1); SELECT COUNT(*) FROM cart WHERE user_id=1")
    check("after checkout a new ACTIVE cart can be created", n == "2", n)

    # --- 2. empty cart / no cart ---
    fresh()
    cart(2, [])
    res, ac, out = run_checkout(normal, [2, 3])      # user 2: empty ACTIVE cart, user 3: no cart at all
    check("empty cart -> 409 'Your cart is empty.'", res and res[0]["status"] == 409 and "empty" in res[0]["msg"], str(res[:1]))
    check("no cart at all -> 409 too", len(res) == 2 and res[1]["status"] == 409, str(res))
    check("nothing was created", counts() == ["0", "0", "0"], str(counts()))
    check("ROLLBACK is logged with a reason", "ROLLBACK reason: Your cart is empty." in out)

    # --- 3. not enough stock: all or nothing, message names the product ---
    fresh()
    sql("UPDATE products SET stock = 2 WHERE product_id = 3")
    cart(1, [(1, 1), (3, 5)])                        # product 1 is fine, product 3 has only 2 left
    st1 = sql("SELECT stock FROM products WHERE product_id=1")
    res, ac, out = run_checkout(normal, [1])
    check("qty > stock -> 409 naming the product and available stock",
          res and res[0]["status"] == 409 and "Only 2 of Power Bank 20000mAh available" in res[0]["msg"], str(res))
    check("nothing changed: no order, stock untouched, cart still ACTIVE",
          counts() == ["0", "0", "0"] and sql("SELECT stock FROM products WHERE product_id=1") == st1 and
          sql("SELECT stock FROM products WHERE product_id=3") == "2" and
          sql("SELECT status FROM cart WHERE user_id=1") == "ACTIVE")
    check("autocommit ON after a rolled back checkout", ac == ["1"] * 4, str(ac))

    # --- 4. forced error after INSERT orders: ROLLBACK removes the order ---
    fresh()
    cart(1, [(1, 2), (2, 1)])
    stock_before = sql("SELECT SUM(stock) FROM products")
    res, ac, out = run_checkout(failing, [1])
    check("TEST_FAIL_CHECKOUT: error page status 500", res and res[0]["status"] == 500, str(res))
    check("the order inserted before the error is rolled back (no orders/items/payments)", counts() == ["0", "0", "0"], str(counts()))
    check("stock unchanged and cart still ACTIVE", sql("SELECT SUM(stock) FROM products") == stock_before and
          sql("SELECT status FROM cart WHERE user_id=1") == "ACTIVE")
    check("log: ROLLBACK reason mentions the test failure", "ROLLBACK reason: Test failure after INSERT orders" in out)
    check("autocommit ON after the forced failure", ac == ["1"] * 4, str(ac))
    # and it works again right after (the pooled connection is clean)
    res, ac, out = run_checkout(normal, [1])
    check("same cart checks out fine afterwards", res and res[0]["status"] == 200, str(res))

    # --- 5. races (rows are held 400 ms after locking so the overlap is certain) ---
    fresh()
    sql("UPDATE products SET stock = 5 WHERE product_id = 1")
    cart(1, [(1, 4)]); cart(2, [(1, 4)])             # two users each want 4 of the last 5
    res, ac, out = run_checkout(slow, [1, 2])
    codes = sorted(r["status"] for r in res)
    check("two buyers, stock 5, each wants 4: exactly one wins (200) and one gets 409", codes == [200, 409], str(res))
    check("stock is 1, never negative, and exactly one order", sql("SELECT stock FROM products WHERE product_id=1") == "1" and
          counts()[0] == "1", str(counts()))
    loser = [r for r in res if r["status"] == 409][0]
    check("the loser is told 'Only 1 of ...'", "Only 1 of" in loser["msg"], loser["msg"])

    fresh()
    cart(1, [(1, 2)])
    res, ac, out = run_checkout(slow, [1, 1])        # same user double-clicks "Place order"
    codes = sorted(r["status"] for r in res)
    check("double submit by one user: one order only", codes == [200, 409] and counts()[0] == "1", str(res))
    check("stock reduced once (not twice)", sql("SELECT stock FROM products WHERE product_id=1") == "38")

    # --- 6. lock wait timeout (1205): retry up to 3 times ---
    sql("SET GLOBAL innodb_lock_wait_timeout = 1")
    fresh()
    cart(1, [(1, 1)])
    blocker = db.popen("START TRANSACTION; SELECT * FROM products WHERE product_id = 1 FOR UPDATE; "
                       "SELECT SLEEP(2); COMMIT;")
    time.sleep(0.5)
    t0 = time.time()
    res, ac, out = run_checkout(normal, [1])
    took = time.time() - t0
    blocker.wait()
    check("lock held 2 s, timeout 1 s: first attempt fails, the retry succeeds", res and res[0]["status"] == 200, str(res))
    check("retry is logged", "retry 2/3" in out and "MySQL error 1205" in out, out[-300:])
    check("exactly one order and stock reduced once", counts()[0] == "1" and sql("SELECT stock FROM products WHERE product_id=1") == "39")

    fresh()
    cart(1, [(1, 1)])
    blocker = db.popen("START TRANSACTION; SELECT * FROM products WHERE product_id = 1 FOR UPDATE; "
                       "SELECT SLEEP(6); COMMIT;")
    time.sleep(0.5)
    res, ac, out = run_checkout(normal, [1])
    blocker.wait()
    check("lock held longer than 3 attempts: friendly 503", res and res[0]["status"] == 503 and "busy" in res[0]["msg"], str(res))
    check("gave up after 3 attempts (logged)", "retry 2/3" in out and "retry 3/3" in out and "gave up after 3 attempts" in out, out[-400:])
    check("nothing changed after giving up", counts() == ["0", "0", "0"] and
          sql("SELECT status FROM cart WHERE user_id=1") == "ACTIVE")
    check("autocommit ON after giving up", ac == ["1"] * 4, str(ac))
    # --- 8. admin hid a product that is already in a cart: checkout refuses and rolls back everything ---
    fresh()
    sql("UPDATE products SET stock = 40 WHERE product_id IN (1, 2)")
    cart(1, [(1, 2), (2, 1)])
    sql("UPDATE products SET is_active = FALSE WHERE product_id = 2")
    res, ac, out = run_checkout(normal, [1])
    name2 = sql("SELECT name FROM products WHERE product_id = 2")
    check("hidden product in the cart -> 409 '<name> is no longer available.'",
          res and res[0]["status"] == 409 and res[0]["msg"] == name2 + " is no longer available.", str(res))
    check("... nothing was created and no stock changed (ROLLBACK)", counts() == ["0", "0", "0"] and
          sql("SELECT stock FROM products WHERE product_id IN (1,2) ORDER BY product_id").replace("\n", "|") == "40|40")
    check("... the cart is still ACTIVE with both items", sql("SELECT status FROM cart WHERE user_id=1") == "ACTIVE" and
          sql("SELECT COUNT(*) FROM cart_items") == "2")
    check("... the ROLLBACK is logged with the reason", "ROLLBACK reason: " + name2 + " is no longer available." in out)
    check("... autocommit is back ON", ac == ["1"] * 4, str(ac))
    sql("UPDATE products SET is_active = TRUE WHERE product_id = 2")
    res, ac, out = run_checkout(normal, [1])
    check("unhide the product and the same cart checks out", res and res[0]["status"] == 200, str(res))
finally:
    db.stop()
    shutil.rmtree(work, ignore_errors=True)         # only the temp folder created above

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
