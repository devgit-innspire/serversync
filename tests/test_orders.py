# test_orders.py - tests order_change() in src/db.cpp (ship / deliver / cancel) on a throwaway MySQL.
# Never reads or writes db.conf and never touches your real database (see throwaway_mysql.py).
#     python tests\test_orders.py
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

work = tempfile.mkdtemp(prefix="serversync_orders_")

def build():
    exe = os.path.join(work, "orders_check.exe")
    for attempt in range(6):            # g++ sometimes runs out of memory when the PC is busy: just try again
        r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "orders_check.cpp"),
                            "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(db.BASE, "include"), "-L" + os.path.join(db.BASE, "lib"),
                            "-lmysql", "-lws2_32", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
        if r.returncode == 0 or "out of memory" not in r.stderr:
            break
        time.sleep(5)
    if r.returncode != 0:
        print(r.stderr[:3000])
        raise SystemExit("build failed")
    shutil.copy(os.path.join(ROOT, "libmysql.dll"), work)
    return exe

def run_jobs(exe, jobs):
    r = subprocess.run([exe] + jobs, capture_output=True, text=True, cwd=work)
    res = []
    for m in re.finditer(r"RESULT job=(\d+) status=(\d+) from=(\S*) msg=(.*)", r.stdout):
        res.append({"status": int(m[2]), "from": m[3], "msg": m[4]})
    return res, re.findall(r"AUTOCOMMIT conn=\d value=(\d)", r.stdout), r.stdout + r.stderr

def fresh():
    db.run(read(r"sql\ecommerce_db.sql"))
    sql("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','x'),('b','b@x.com','x'),('c','c@x.com','x')")
    sql("UPDATE products SET stock = 10")

def make_order(user, items, status="PLACED", pay="PENDING"):
    """a finished checkout: CHECKED_OUT cart, order (+ items at today's price), payment. Stock is NOT touched here."""
    sql("INSERT INTO cart (user_id, status) VALUES (%d, 'CHECKED_OUT')" % user)
    cid = sql("SELECT MAX(cart_id) FROM cart")
    total = sum(int(q) * float(sql("SELECT price FROM products WHERE product_id=%d" % p)) for p, q in items)
    sql("INSERT INTO orders (user_id, cart_id, total, status) VALUES (%d, %s, %.2f, '%s')" % (user, cid, total, status))
    oid = sql("SELECT MAX(order_id) FROM orders")
    for p, q in items:
        sql("INSERT INTO order_items (order_id, product_id, quantity, price) SELECT %s, %d, %d, price FROM products WHERE product_id=%d" % (oid, p, q, p))
    sql("INSERT INTO payments (order_id, amount, method, status) VALUES (%s, %.2f, 'COD', '%s')" % (oid, total, pay))
    return oid

def state(oid):
    return sql("SELECT o.status, p.status, IF(p.paid_at IS NULL, 'NULL', 'SET') FROM orders o JOIN payments p ON p.order_id=o.order_id WHERE o.order_id=%s" % oid).split("\t")

def stocks(*pids):
    return [sql("SELECT stock FROM products WHERE product_id=%d" % p) for p in pids]

try:
    db.start()
    exe = build()
    sql_ok = lambda: None

    # ---- PLACED -> SHIPPED ----
    fresh()
    o1 = make_order(1, [(1, 2), (2, 1)])
    res, ac, out = run_jobs(exe, ["%s:SHIPPED:3:1" % o1])
    check("PLACED -> SHIPPED: 200, old status reported", res and res[0]["status"] == 200 and res[0]["from"] == "PLACED", str(res))
    check("... order SHIPPED, payment still PENDING without paid_at (COD pays on delivery)", state(o1) == ["SHIPPED", "PENDING", "NULL"], str(state(o1)))
    check("... stock untouched", stocks(1, 2) == ["10", "10"])
    check("... autocommit ON on every pooled connection", ac == ["1"] * 4, str(ac))

    # ---- SHIPPED -> DELIVERED: payment PAID in the same transaction ----
    res, ac, out = run_jobs(exe, ["%s:DELIVERED:3:1" % o1])
    check("SHIPPED -> DELIVERED: 200", res and res[0]["status"] == 200 and res[0]["from"] == "SHIPPED", str(res))
    check("... order DELIVERED and payment PAID with paid_at set", state(o1) == ["DELIVERED", "PAID", "SET"], str(state(o1)))
    check("... paid_at is 'now' (within a minute)", int(sql("SELECT TIMESTAMPDIFF(SECOND, paid_at, NOW()) FROM payments WHERE order_id=%s" % o1)) < 60)

    # ---- nothing changes after DELIVERED / CANCELLED ----
    for to in ("SHIPPED", "DELIVERED", "CANCELLED"):
        res, ac, out = run_jobs(exe, ["%s:%s:3:1" % (o1, to)])
        check("DELIVERED -> %s: 400 'Not allowed'" % to, res and res[0]["status"] == 400 and "Not allowed" in res[0]["msg"], str(res))
    check("... and nothing changed (still DELIVERED / PAID), stock untouched", state(o1) == ["DELIVERED", "PAID", "SET"] and stocks(1, 2) == ["10", "10"])

    # ---- not allowed jumps ----
    o2 = make_order(1, [(3, 1)])
    for to in ("DELIVERED", "PLACED", "PENDING", "NOPE"):
        res, ac, out = run_jobs(exe, ["%s:%s:3:1" % (o2, to)])
        check("PLACED -> %s: 400 not allowed" % to, res and res[0]["status"] == 400, str(res))
    check("... order still PLACED / PENDING", state(o2) == ["PLACED", "PENDING", "NULL"])

    # ---- cancel: ONE transaction: stock back, order + payment CANCELLED ----
    fresh()
    o3 = make_order(1, [(1, 2), (2, 3), (5, 1)])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o3])
    check("cancel a PLACED order (admin): 200", res and res[0]["status"] == 200 and res[0]["from"] == "PLACED", str(res))
    check("... each item's quantity is added back (10+2, 10+3, 10+1)", stocks(1, 2, 5) == ["12", "13", "11"], str(stocks(1, 2, 5)))
    check("... order CANCELLED and payment CANCELLED", state(o3) == ["CANCELLED", "CANCELLED", "NULL"], str(state(o3)))
    check("... log line: [ORDER] order N CANCELLED by user 3, stock restored", "[ORDER] order %s CANCELLED by user 3, stock restored" % o3 in out, out[-300:])
    check("... other products untouched", stocks(3, 4) == ["10", "10"])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o3])
    check("cancelling twice: 400, stock NOT added a second time", res and res[0]["status"] == 400 and stocks(1, 2, 5) == ["12", "13", "11"], str(res))

    o4 = make_order(2, [(1, 4)], status="SHIPPED")
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o4])
    check("cancel a SHIPPED order (admin): 200 and stock restored (12+4)", res and res[0]["status"] == 200 and stocks(1) == ["16"] and state(o4)[:2] == ["CANCELLED", "CANCELLED"], str(res))
    sql("UPDATE products SET is_active = FALSE WHERE product_id = 2")
    o5 = make_order(2, [(2, 5)])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o5])
    check("cancel works for an order with a hidden product (stock 13+5)", res and res[0]["status"] == 200 and stocks(2) == ["18"], str(res))

    # ---- customer cancel: own PLACED order only ----
    fresh()
    mine = make_order(1, [(1, 2)])
    theirs = make_order(2, [(1, 3)])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:1:0" % theirs])
    check("customer cancels someone else's order: 404 (looks like it does not exist)", res and res[0]["status"] == 404 and "not found" in res[0]["msg"].lower(), str(res))
    check("... nothing changed", state(theirs) == ["PLACED", "PENDING", "NULL"] and stocks(1) == ["10"])
    res, ac, out = run_jobs(exe, ["%s:SHIPPED:1:0" % mine])
    check("customer tries to SHIP / DELIVER his own order: 400", res and res[0]["status"] == 400, str(res))
    res, ac, out = run_jobs(exe, ["%s:DELIVERED:1:0" % mine])
    check("... DELIVERED too", res and res[0]["status"] == 400 and state(mine) == ["PLACED", "PENDING", "NULL"])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:1:0" % mine])
    check("customer cancels his own PLACED order: 200, stock back (10+2)", res and res[0]["status"] == 200 and stocks(1) == ["12"] and state(mine)[:2] == ["CANCELLED", "CANCELLED"], str(res))
    shipped = make_order(1, [(3, 1)], status="SHIPPED")
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:1:0" % shipped])
    check("customer cannot cancel a SHIPPED order: 400, nothing changed", res and res[0]["status"] == 400 and state(shipped)[0] == "SHIPPED" and stocks(3) == ["10"], str(res))
    res, ac, out = run_jobs(exe, ["999999:CANCELLED:1:1"])
    check("unknown order: 404", res and res[0]["status"] == 404, str(res))

    # ---- stale page: expected status ----
    fresh()
    o6 = make_order(1, [(1, 1)])
    res, ac, out = run_jobs(exe, ["%s:SHIPPED:3:1:PLACED" % o6])
    check("expected_from matches: 200", res and res[0]["status"] == 200, str(res))
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1:PLACED" % o6])
    check("expected_from is stale (page said PLACED, now SHIPPED): 409, nothing changed", res and res[0]["status"] == 409 and state(o6)[0] == "SHIPPED" and stocks(1) == ["10"], str(res))

    # ---- races ----
    fresh()
    o7 = make_order(1, [(1, 2)])
    res, ac, out = run_jobs(exe, ["%s:SHIPPED:1:1:PLACED" % o7, "%s:SHIPPED:2:1:PLACED" % o7])
    check("two admins press 'Ship' together: exactly one 200, the other 409/400, status SHIPPED once",
          sorted(r["status"] for r in res)[0] == 200 and sorted(r["status"] for r in res)[1] in (400, 409) and state(o7)[0] == "SHIPPED", str(res))
    fresh()
    o8 = make_order(1, [(1, 2), (2, 2)])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:1:1" % o8, "%s:CANCELLED:1:0" % o8, "%s:CANCELLED:2:1" % o8])
    check("admin + customer + admin cancel the same order at once: exactly ONE succeeds",
          sorted(r["status"] for r in res) == [200, 400, 400], str(res))
    check("... stock restored exactly once (12 and 12), never twice", stocks(1, 2) == ["12", "12"], str(stocks(1, 2)))
    check("... autocommit ON everywhere", ac == ["1"] * 4, str(ac))
    fresh()
    a = make_order(1, [(1, 1), (2, 1)])
    b = make_order(2, [(2, 1), (1, 1)])
    c = make_order(3, [(1, 1)])
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % a, "%s:CANCELLED:3:1" % b, "%s:CANCELLED:3:1" % c, "%s:SHIPPED:3:1" % a])
    check("3 cancels sharing products (opposite item order) at once: all fine, no deadlock error", [r["status"] for r in res][:3] == [200, 200, 200] or
          sorted(r["status"] for r in res[:3]).count(200) == 3, str(res))
    check("... stock of product 1 is 10+3, product 2 is 10+2", stocks(1, 2) == ["13", "12"], str(stocks(1, 2)))

    # ---- all-or-nothing: a failure in the LAST step rolls back the stock and the status ----
    fresh()
    o9 = make_order(1, [(1, 2), (2, 3)])
    sql("ALTER TABLE payments DROP CHECK chk_payments_status")
    sql("ALTER TABLE payments ADD CONSTRAINT chk_payments_status CHECK (status IN ('PENDING','PAID'))")      # payment CANCELLED is now refused
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o9])
    check("error in the last step (payment CANCELLED refused by a CHECK): 500", res and res[0]["status"] == 500, str(res))
    check("... ROLLBACK: stock NOT restored, order still PLACED, payment still PENDING", stocks(1, 2) == ["10", "10"] and state(o9) == ["PLACED", "PENDING", "NULL"], str(state(o9)) + str(stocks(1, 2)))
    check("... the rollback is logged with the MySQL error", "ROLLBACK reason" in out and "MySQL error 3819" in out, out[-300:])
    check("... autocommit is back ON", ac == ["1"] * 4, str(ac))
    sql("ALTER TABLE payments DROP CHECK chk_payments_status")
    sql("ALTER TABLE payments ADD CONSTRAINT chk_payments_status CHECK (status IN ('PENDING', 'PAID', 'REFUNDED', 'CANCELLED'))")
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o9])
    check("same order cancels fine once the problem is gone (connection was clean)", res and res[0]["status"] == 200 and stocks(1, 2) == ["12", "13"], str(res))
    # DELIVERED: payment must be updated too, else rollback
    o10 = make_order(1, [(3, 1)], status="SHIPPED")
    sql("DELETE FROM payments WHERE order_id = %s" % o10)
    res, ac, out = run_jobs(exe, ["%s:DELIVERED:3:1" % o10])
    check("DELIVERED with a missing payment row: 500 and the order is NOT marked DELIVERED (ROLLBACK)", res and res[0]["status"] == 500 and sql("SELECT status FROM orders WHERE order_id=%s" % o10) == "SHIPPED", str(res))

    # ---- audit: admin changes write one admin_actions row in the SAME transaction; customer cancels write none ----
    fresh()
    oa = make_order(1, [(1, 1)])
    run_jobs(exe, ["%s:SHIPPED:3:1" % oa])
    rowsa = sql("SELECT admin_id, action, details FROM admin_actions").split("\n")
    check("admin ships an order: one audit row 'ORDER_STATUS order N PLACED->SHIPPED' by that admin", rowsa == ["3\tORDER_STATUS\torder %s PLACED->SHIPPED" % oa], str(rowsa))
    run_jobs(exe, ["%s:DELIVERED:3:1" % oa])
    check("... deliver adds a second row", sql("SELECT COUNT(*) FROM admin_actions") == "2")
    run_jobs(exe, ["%s:SHIPPED:3:1" % oa])
    check("a refused change (not allowed) writes no audit row", sql("SELECT COUNT(*) FROM admin_actions") == "2")
    ob = make_order(2, [(1, 1)])
    run_jobs(exe, ["%s:CANCELLED:2:0" % ob])
    check("a CUSTOMER cancelling his own order writes no audit row (it is not an admin action)", sql("SELECT COUNT(*) FROM admin_actions") == "2")
    oc = make_order(2, [(2, 1)])
    sql("RENAME TABLE admin_actions TO admin_actions_hidden")
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % oc])
    sql("RENAME TABLE admin_actions_hidden TO admin_actions")
    check("audit row cannot be written: the admin's cancel is rolled back too (stock, status, payment unchanged)",
          res and res[0]["status"] == 500 and state(oc)[:2] == ["PLACED", "PENDING"] and stocks(2) == ["10"], str(res))

    # ---- lock wait timeout (1205): retried 3 times, then a friendly 503 ----
    sql("SET GLOBAL innodb_lock_wait_timeout = 1")
    fresh()
    o11 = make_order(1, [(1, 2)])
    blocker = db.popen("START TRANSACTION; SELECT * FROM orders WHERE order_id = %s FOR UPDATE; SELECT SLEEP(6); COMMIT;" % o11)
    time.sleep(0.5)
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o11])
    blocker.wait()
    check("order row locked for 6 s: 503 after 3 attempts", res and res[0]["status"] == 503 and "busy" in res[0]["msg"], str(res))
    check("... retries are logged", "retry 2/3" in out and "retry 3/3" in out and "gave up after 3 attempts" in out, out[-400:])
    check("... nothing changed (stock 10, order PLACED) and autocommit ON", stocks(1) == ["10"] and state(o11)[0] == "PLACED" and ac == ["1"] * 4)
    fresh()
    o12 = make_order(1, [(1, 2)])
    blocker = db.popen("START TRANSACTION; SELECT * FROM orders WHERE order_id = %s FOR UPDATE; SELECT SLEEP(2); COMMIT;" % o12)
    time.sleep(0.5)
    res, ac, out = run_jobs(exe, ["%s:CANCELLED:3:1" % o12])
    blocker.wait()
    check("lock held 2 s, timeout 1 s: first try fails, the retry succeeds (200, stock 12)", res and res[0]["status"] == 200 and stocks(1) == ["12"], str(res))
    check("... 'retry 2/3' and error 1205 are logged", "retry 2/3" in out and "MySQL error 1205" in out, out[-300:])

    # ---- the code has the safeguards the task asked for ----
    dbsrc = read(r"src\db.cpp")
    check("source: order row is read FOR UPDATE, status UPDATE has AND status = <old>, items in product_id order",
          "WHERE order_id = ? FOR UPDATE" in dbsrc and "UPDATE orders SET status = ? WHERE order_id = ? AND status = ?" in dbsrc and
          "ORDER BY product_id" in dbsrc and "stock = stock + ?" in dbsrc)
    check("source: same deadlock retry and autocommit reset as checkout", dbsrc.count("CHECKOUT_RETRIES") >= 4 and dbsrc.count("db_end(m);") >= 2)
finally:
    shutil.rmtree(work, ignore_errors=True)
    db.stop()

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
