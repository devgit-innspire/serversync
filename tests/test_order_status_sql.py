# test_order_status_sql.py - (1) audit: every status / reason value used in src\*.cpp is allowed by the CHECK
# constraints in sql\ecommerce_db.sql, (2) the optional ALTER script on an OLD database with data.
# Uses a throwaway MySQL (throwaway_mysql.py). Never reads or writes db.conf, never touches your real database.
#     python tests\test_order_status_sql.py
import glob, os, re, sys
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

schema = read(r"sql\ecommerce_db.sql")
FINAL = ["PLACED", "PENDING", "CONFIRMED", "SHIPPED", "DELIVERED", "CANCELLED"]
OLD = ["PLACED", "SHIPPED", "DELIVERED", "CANCELLED"]          # what an older database has

# ---------- 1. audit: values used by the code vs CHECK constraints ----------
def allowed(constraint):
    m = re.search(r"CONSTRAINT %s CHECK \(\w+ IN \(([^)]*)\)\)" % constraint, schema)
    return re.findall(r"'(\w+)'", m.group(1)) if m else []

allow = {"orders.status": allowed("chk_orders_status"), "payments.status": allowed("chk_payments_status"),
         "cart.status": allowed("chk_cart_status"), "user_sessions.end_reason": allowed("chk_sessions_end_reason")}
check("orders.status CHECK is " + "/".join(FINAL), allow["orders.status"] == FINAL, str(allow["orders.status"]))
check("orders.status DEFAULT is PLACED (what the code inserts)", re.search(r"status\s+VARCHAR\(20\) NOT NULL DEFAULT 'PLACED',\s+created_at", schema))

used = []     # (column, value, file)
for path in glob.glob(os.path.join(ROOT, "src", "*.cpp")):
    text = open(path, encoding="utf-8").read()
    name = os.path.basename(path)
    for m in re.finditer(r'"([^"]*\b(?:status|end_reason)\s*=\s*\'(\w+)\'[^"]*)"', text):
        stmt, value = m.group(1), m.group(2)
        col = "user_sessions.end_reason" if "end_reason" in stmt else (
              "cart.status" if re.search(r"\bcart\b", stmt) and "user_sessions" not in stmt and "orders" not in stmt else None)
        if col:
            used.append((col, value, name))
    for m in re.finditer(r"INSERT INTO orders \(user_id, cart_id, total, status\) \"\s*\"SELECT [^\"]*'(\w+)'", text):
        used.append(("orders.status", m.group(1), name))
    for m in re.finditer(r"INSERT INTO payments \(order_id, amount, method, status, paid_at\) \"\s*\"SELECT order_id, total, '(\w+)', '(\w+)'", text):
        used.append(("payments.method", m.group(1), name))
        used.append(("payments.status", m.group(2), name))

check("the audit found the values the code uses (orders, payments, cart, sessions)",
      {c for c, v, f in used} >= {"orders.status", "payments.status", "payments.method", "cart.status",
                                  "user_sessions.end_reason"}, str(sorted({c for c, v, f in used})))
for col, value, f in sorted(set(used)):
    if col == "payments.method":
        continue          # payments.method has no CHECK (free text, default 'COD'): reported separately
    check("%s = '%s' (used in %s) is allowed by the schema" % (col, value, f), value in allow[col], "allowed: %s" % allow[col])
check("checkout inserts orders.status 'PLACED'", ("orders.status", "PLACED", "db.cpp") in used, str(sorted(set(used))))
check("an older database's list (PLACED/SHIPPED/DELIVERED/CANCELLED) already allows what the code inserts",
      "PLACED" in OLD)
check("payments.method has no CHECK (code inserts 'COD', schema default is 'COD')",
      "chk_payments_method" not in schema and ("payments.method", "COD", "db.cpp") in used)

# ---------- 2. the optional ALTER script on an old database that has data ----------
old_schema = schema.replace("DEFAULT 'PLACED',\n    created_at", "DEFAULT 'PLACED',\n    created_at", 1)
old_schema = re.sub(r"CHECK \(status IN \('PLACED', 'PENDING', 'CONFIRMED', 'SHIPPED', 'DELIVERED', 'CANCELLED'\)\)",
                    "CHECK (status IN ('PLACED', 'SHIPPED', 'DELIVERED', 'CANCELLED'))", old_schema)
assert "('PLACED', 'SHIPPED', 'DELIVERED', 'CANCELLED')" in old_schema

def sql(q, **kw):
    return db.run(q, "ecommerce_db", **kw)

def order(status, n):
    sql("INSERT INTO cart (user_id, status) VALUES (1,'CHECKED_OUT')")
    cid = sql("SELECT MAX(cart_id) FROM cart")
    return sql("INSERT INTO orders (user_id, cart_id, total, status) VALUES (1, %s, %d.00, '%s')" % (cid, n * 100, status),
               expect_error=True)

try:
    db.start()

    db.run(old_schema)
    sql("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','secret1')")
    for i, st in enumerate(["PLACED", "PLACED", "SHIPPED", "DELIVERED", "CANCELLED"], start=1):
        failed, err = order(st, i)
        assert not failed, err
    failed, err = order("PLACED", 6)
    check("OLD database accepts what the code inserts: status 'PLACED'", not failed, err[:80])
    failed, err = order("CONFIRMED", 7)
    check("OLD database rejects 'CONFIRMED' (the earlier failure)", failed and "chk_orders_status" in err, err[:80])

    db.run(read(r"sql\alter_002_order_status_values.sql"))
    rows = sql("SELECT status, COUNT(*) FROM orders GROUP BY status ORDER BY status").replace("\n", "|")
    check("ALTER keeps all data and renames nothing (PLACED stays PLACED)",
          rows == "CANCELLED\t1|DELIVERED\t1|PLACED\t3|SHIPPED\t1", rows)
    for st in FINAL:
        failed, err = order(st, 9)
        check("after the ALTER: '%s' is accepted" % st, not failed, err[:80])
    for bad in ["FOO", "SHIPED"]:
        failed, err = order(bad, 9)
        check("after the ALTER: '%s' is rejected" % bad, failed and "3819" in err, err[:80])

    # fresh database from ecommerce_db.sql: same behaviour, no migration
    db.run(schema)
    sql("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','secret1')")
    for st in FINAL:
        failed, err = order(st, 1)
        check("fresh ecommerce_db.sql accepts '%s'" % st, not failed, err[:80])
    sql("INSERT INTO cart (user_id, status) VALUES (1,'CHECKED_OUT')")
    cid = sql("SELECT MAX(cart_id) FROM cart")
    sql("INSERT INTO orders (user_id,cart_id,total) VALUES (1, %s, 1.00)" % cid)
    check("fresh schema: default status is PLACED", sql("SELECT status FROM orders WHERE cart_id=%s" % cid) == "PLACED")
finally:
    db.stop()

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
