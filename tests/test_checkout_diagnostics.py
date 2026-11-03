# test_checkout_diagnostics.py - what the log shows when checkout hits a CHECK constraint failure (error 3819),
# and that the binding is right. Uses a throwaway database (throwaway_mysql.py); never reads or writes db.conf and
# never touches your real database.
#     python tests\test_checkout_diagnostics.py
import os, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as db

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def sql(q):
    return db.run(q, "ecommerce_db")

work = tempfile.mkdtemp(prefix="serversync_diag_")
try:
    # build with DEBUG_SQL = 1 (a copy of include/ is patched; the real common.h is untouched)
    inc = os.path.join(work, "include")
    shutil.copytree(os.path.join(ROOT, "include"), inc)
    path = os.path.join(inc, "common.h")
    text = open(path, encoding="utf-8").read()
    assert "const int DEBUG_SQL = 0;" in text, "DEBUG_SQL must default to 0 in common.h"
    open(path, "w", encoding="utf-8").write(text.replace("const int DEBUG_SQL = 0;", "const int DEBUG_SQL = 1;"))
    exe = os.path.join(work, "checkout_check.exe")
    r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "checkout_check.cpp"),
                        "-I" + inc, "-I" + os.path.join(db.BASE, "include"), "-L" + os.path.join(db.BASE, "lib"),
                        "-lmysql", "-lws2_32", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[:2000]
    shutil.copy(os.path.join(ROOT, "libmysql.dll"), work)

    def checkout(user):
        p = subprocess.run([exe, str(user)], capture_output=True, text=True, cwd=work)
        return p.stdout + p.stderr

    schema = open(os.path.join(ROOT, "sql", "ecommerce_db.sql"), encoding="utf-8").read()
    prep = ("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','secret1'); INSERT INTO cart (user_id) VALUES (1);"
            "INSERT INTO cart_items (cart_id, product_id, quantity) VALUES (1, 1, 2)")

    db.start()

    # A) a database whose orders CHECK does not allow PLACED -> checkout fails; the log must explain why
    db.run(schema)
    sql(prep)
    sql("ALTER TABLE orders DROP CHECK chk_orders_status; ALTER TABLE orders ADD CONSTRAINT chk_orders_status "
        "CHECK (status IN ('PENDING','CONFIRMED','SHIPPED','DELIVERED','CANCELLED'))")
    out = checkout(1)
    print("---- log from the failing checkout ----")
    print("\n".join(l for l in out.splitlines() if "[DB]" in l or "CHECKOUT" in l or "RESULT" in l))
    print("----")
    check("checkout fails with chk_orders_status violated", "Check constraint 'chk_orders_status' is violated" in out)
    check("log shows the SQL with the 'PLACED' literal", "SQL: INSERT INTO orders" in out and "'PLACED'" in out)
    check("log shows each bound param with value and length (DEBUG_SQL = 1)",
          "param 1 = '1' (len 1, bound length 1)" in out and "param 3 = '1' (len 1, bound length 1)" in out)
    check("exactly 3 params for the orders insert (status is not one of them)",
          "param 3 =" in out.split("SQL: INSERT INTO orders")[1].split("execute failed")[0] and
          "param 4" not in out.split("SQL: INSERT INTO orders")[1].split("execute failed")[0])
    check("log shows the REAL rule in the database",
          "the rule 'chk_orders_status' is:" in out and "PLACED" not in out.split("the rule 'chk_orders_status' is:")[1].split("\n")[0])
    check("nothing was saved (rolled back)", sql("SELECT COUNT(*) FROM orders") == "0")

    # B) the schema file's database: the same code succeeds and stores PLACED
    db.run(schema)
    sql(prep)
    out = checkout(1)
    check("with ecommerce_db.sql's CHECK the same code succeeds", "status=200" in out and "COMMIT" in out, out[-200:])
    check("order row has status PLACED", sql("SELECT status FROM orders") == "PLACED")
finally:
    db.stop()
    shutil.rmtree(work, ignore_errors=True)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
