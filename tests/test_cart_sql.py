# test_cart_sql.py - checks the cart SQL from db.cpp on a throwaway MySQL (see throwaway_mysql.py):
# atomic add with the row alias, the MAX_CART_QTY cap, the lost-update demo, and the one-ACTIVE-cart race (1062).
#     python tests\test_cart_sql.py
import os, sys
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

# the exact statement built by cart_add() in db.cpp (MAX_CART_QTY = 10)
src = read(r"src\db.cpp")
assert "AS new_item" in src and "LEAST(cart_items.quantity + new_item.quantity" in src
assert "const int MAX_CART_QTY = 10;" in read(r"include\common.h")
ADD = ("INSERT INTO cart_items (cart_id, product_id, quantity) VALUES (1, 1, %d) AS new_item "
       "ON DUPLICATE KEY UPDATE quantity = LEAST(cart_items.quantity + new_item.quantity, 10);")

def qty(cart=1, product=1):
    return db.run("SELECT IFNULL(SUM(quantity),0) FROM cart_items WHERE cart_id=%d AND product_id=%d" % (cart, product),
                  "ecommerce_db")

try:
    db.start()
    db.run(read(r"sql\ecommerce_db.sql"))
    db.run("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','secret1'),('b','b@x.com','secret1');"
           "INSERT INTO cart (user_id) VALUES (1);", "ecommerce_db")

    # --- atomic add + cap ---
    db.run(ADD % 3, "ecommerce_db");  check("new item: qty 3", qty() == "3", qty())
    db.run(ADD % 4, "ecommerce_db");  check("same product again: 3 + 4 = 7 (one row, not two)", qty() == "7", qty())
    rows = db.run("SELECT COUNT(*) FROM cart_items WHERE cart_id=1", "ecommerce_db")
    check("UNIQUE(cart_id, product_id) keeps a single row", rows == "1", rows)
    db.run(ADD % 5, "ecommerce_db");  check("7 + 5 is capped at 10", qty() == "10", qty())
    db.run(ADD % 1, "ecommerce_db");  check("stays at 10", qty() == "10", qty())
    failed, err = db.run("UPDATE cart_items SET quantity = 0 WHERE cart_id=1", "ecommerce_db", expect_error=True)
    check("CHECK(quantity > 0) rejects qty 0 (that is why qty 0 means DELETE)", failed and "3819" in err, err[:70])

    # --- atomic add under concurrency ---
    db.run("DELETE FROM cart_items WHERE cart_id=1", "ecommerce_db")
    procs = [db.popen(ADD % 1) for _ in range(8)]
    for p in procs:
        p.wait()
    check("8 concurrent adds of 1 -> exactly 8 (no lost update)", qty() == "8", qty())

    # --- the lost-update problem itself, made deterministic with SLEEP between read and write ---
    db.run("DELETE FROM cart_items WHERE cart_id=1;"
           "INSERT INTO cart_items (cart_id, product_id, quantity) VALUES (1, 2, 2);", "ecommerce_db")
    naive = ("SELECT quantity INTO @q FROM cart_items WHERE cart_id=1 AND product_id=2; SELECT SLEEP(1);"
             "UPDATE cart_items SET quantity = @q + 1 WHERE cart_id=1 AND product_id=2;")
    a, b = db.popen(naive), db.popen(naive)
    a.wait(); b.wait()
    check("NAIVE read-modify-write: two +1 requests give 3 instead of 4 (lost update)", qty(1, 2) == "3", qty(1, 2))

    db.run("UPDATE cart_items SET quantity = 2 WHERE cart_id=1 AND product_id=2", "ecommerce_db")
    atomic = ("SELECT SLEEP(1);" + ADD.replace("(1, 1, %d)", "(1, 2, 1)"))
    a, b = db.popen(atomic), db.popen(atomic)
    a.wait(); b.wait()
    check("ATOMIC add: same two requests give 4 (nothing lost)", qty(1, 2) == "4", qty(1, 2))

    # --- one ACTIVE cart per user: the 1062 race in get_active_cart() ---
    failed, err = db.run("INSERT INTO cart (user_id) VALUES (1)", "ecommerce_db", expect_error=True)
    check("second ACTIVE cart for user 1 -> error 1062", failed and "1062" in err, err[:70])
    n = db.run("SELECT COUNT(*) FROM cart WHERE user_id=1 AND status='ACTIVE'", "ecommerce_db")
    check("still exactly one ACTIVE cart", n == "1", n)
    db.run("UPDATE cart SET status='CHECKED_OUT' WHERE cart_id=1;INSERT INTO cart (user_id) VALUES (1);", "ecommerce_db")
    n = db.run("SELECT COUNT(*) FROM cart WHERE user_id=1", "ecommerce_db")
    check("after checkout a new ACTIVE cart is allowed (2 carts total)", n == "2", n)
    a, b = db.popen("INSERT INTO cart (user_id) VALUES (2);"), db.popen("INSERT INTO cart (user_id) VALUES (2);")
    a.wait(); b.wait()
    codes = sorted([a.returncode, b.returncode])
    n = db.run("SELECT COUNT(*) FROM cart WHERE user_id=2", "ecommerce_db")
    check("two simultaneous creates for user 2: one wins, one gets 1062", codes == [0, 1] and n == "1",
          "exit codes %s, carts %s" % (codes, n))

    # --- count query and cart items query shapes used by the pages ---
    out = db.run("SELECT COALESCE(SUM(ci.quantity), 0) FROM cart c LEFT JOIN cart_items ci ON ci.cart_id = c.cart_id "
                 "WHERE c.user_id = 2 AND c.status = 'ACTIVE'", "ecommerce_db")
    check("cart_count for an empty ACTIVE cart is 0", out == "0", out)
    out = db.run("SELECT COALESCE(SUM(ci.quantity), 0) FROM cart c LEFT JOIN cart_items ci ON ci.cart_id = c.cart_id "
                 "WHERE c.user_id = 99 AND c.status = 'ACTIVE'", "ecommerce_db")
    check("cart_count for a user with no cart is 0", out == "0", out)
finally:
    db.stop()

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
