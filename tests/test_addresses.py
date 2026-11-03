# test_addresses.py - tests the shipping address functions and the checkout address snapshot (src/db.cpp) on a throwaway MySQL.
# Never reads or writes db.conf and never touches your real database (see throwaway_mysql.py).
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as t

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MYSQL_DIR = r"C:\Program Files\MySQL\MySQL Server 8.0"
exe = os.path.join(os.environ.get("TEMP", "."), "addresses_check.exe")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    if not ok:
        fails += 1


r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "addresses_check.cpp"),
                    "-I" + os.path.join(ROOT, "include"), "-I" + MYSQL_DIR + "\\include", "-L" + MYSQL_DIR + "\\lib",
                    "-lmysql", "-lws2_32", "-lbcrypt"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr)
    sys.exit(1)

t.start()
try:
    t.run(open(os.path.join(ROOT, "sql", "ecommerce_db.sql"), encoding="utf-8").read())
    t.run("""INSERT INTO users(email,name,password) VALUES('a@x.in','A','x'),('b@x.in','B','x');
             INSERT INTO products(name,description,price,stock,category_id) VALUES('Widget','w',100.00,50,1);
             INSERT INTO cart(user_id) VALUES(1);
             INSERT INTO cart_items(cart_id,product_id,quantity) SELECT 1, MIN(product_id), 2 FROM products WHERE name='Widget';""",
          db="ecommerce_db")
    env = dict(os.environ)
    env["PATH"] = MYSQL_DIR + "\\lib;" + env["PATH"]
    p = subprocess.run([exe], capture_output=True, text=True, env=env)
    out = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition(" ")
        out[k] = v
    print(p.stderr)

    check("good address passes and is trimmed", out.get("CHECK_OK") == "[] first=[Asha]", out.get("CHECK_OK"))
    check("phone must be 10 digits", "10 digits" in out["CHECK_PHONE"] and "10 digits" in out["CHECK_PHONE_LETTERS"])
    check("pincode cannot start with 0 / must be 6 digits", "6 digits" in out["CHECK_PIN_ZERO"] and "6 digits" in out["CHECK_PIN_SHORT"])
    check("bad type, empty name, missing state, too long line1",
          "Home, Work or Other" in out["CHECK_TYPE"] and "required" in out["CHECK_NAME"] and "state" in out["CHECK_STATE"]
          and "at most 150" in out["CHECK_LONG"])
    check("two addresses saved", out["SAVE1"].startswith("200") and out["SAVE2"].startswith("200"), out["SAVE1"] + out["SAVE2"])
    check("unknown state id -> 400", out["SAVE_BAD_STATE"] == "400", out["SAVE_BAD_STATE"])
    check("set default works; another user's address -> 404", out["DEFAULT2"] == "200" and out["DEFAULT_OTHER_USER"] == "404")
    check("edit: own 200, other user's 404", out["EDIT_OWN"] == "200" and out["EDIT_OTHER_USER"] == "404")
    check("limit of 10 active addresses (400)", out.get("LIMIT_STATUS", "").startswith("400"), out.get("LIMIT_STATUS"))
    check("delete: foreign 404, own 200, again 404",
          (out["DELETE_FOREIGN"], out["DELETE_OWN"], out["DELETE_AGAIN"]) == ("404", "200", "404"))
    n_active = t.run("SELECT COUNT(*) FROM addresses WHERE user_id=1 AND is_active=1", db="ecommerce_db")
    n_all = t.run("SELECT COUNT(*) FROM addresses WHERE user_id=1", db="ecommerce_db")
    check("delete is soft: row kept, only 9 active of 10", (n_active, n_all) == ("9", "10"), (n_active, n_all))
    defaults = t.run("SELECT COUNT(*) FROM addresses WHERE user_id=1 AND is_default=1 AND is_active=1", db="ecommerce_db")
    check("exactly one default for user 1", defaults == "1", defaults)
    check("checkout with missing / deleted / other user's address -> 404",
          out["CO_MISSING"] == "404" and out["CO_DELETED"] == "404" and out["CO_OTHER_USERS"] == "404")
    check("checkout with own address -> 200", out["CO_OK"].startswith("200"), out["CO_OK"])
    ids = out["IDS"].split()
    # the three failed checkouts ROLLED BACK: the only order is the successful one (order id 1, not 4)
    check("failed checkouts left no order behind", t.run("SELECT GROUP_CONCAT(order_id) FROM orders", db="ecommerce_db") == ids[1] == "1")
    snap = t.run("SELECT first_name,city,state_name,pincode FROM order_addresses WHERE order_id=%s" % ids[1], db="ecommerce_db")
    check("order_addresses snapshot written (state as text)", snap.split("\t")[1:3] == ["Mumbai", "Uttar Pradesh"], snap)
    t.run("UPDATE addresses SET city='Delhi', is_active=0 WHERE address_id=%s" % ids[0], db="ecommerce_db")
    snap2 = t.run("SELECT city FROM order_addresses WHERE order_id=%s" % ids[1], db="ecommerce_db")
    check("editing / deleting the address does not change the old order", snap2 == "Mumbai", snap2)
finally:
    t.stop()
print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
