# test_schema_file.py - imports sql/ecommerce_db.sql into a throwaway MySQL (see throwaway_mysql.py; never your real database,
# never db.conf) with the exact flag from sql/README.md, and checks that it holds everything the old alter_00x files added.
#     python tests\test_schema_file.py
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as t

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SQL = os.path.join(ROOT, "sql", "ecommerce_db.sql")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    if not ok:
        fails += 1


def q(sql):
    return t.run(sql, db="ecommerce_db")


def import_file():
    with open(SQL, "rb") as f:
        return subprocess.run([t.MYSQL] + t.CONN + ["--default-character-set=utf8mb4"], stdin=f, capture_output=True, text=True)


t.start()
try:
    r = import_file()
    check("import with --default-character-set=utf8mb4 works", r.returncode == 0, r.stderr)
    r = import_file()
    check("importing a second time works too (DROP DATABASE IF EXISTS)", r.returncode == 0, r.stderr)

    check("database is utf8mb4", t.run("SELECT DEFAULT_CHARACTER_SET_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME='ecommerce_db'") == "utf8mb4")
    tables = set(q("SHOW TABLES").split())
    want = {"users", "user_sessions", "categories", "products", "cart", "cart_items", "orders", "order_status_history", "order_items",
            "payments", "request_logs", "admin_actions", "states", "addresses", "order_addresses"}
    check("all 15 tables exist", tables == want, tables ^ want)
    check("all tables are InnoDB utf8mb4", q("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='ecommerce_db' AND "
                                              "(ENGINE<>'InnoDB' OR TABLE_COLLATION NOT LIKE 'utf8mb4%')") == "0")
    check("seed: 15 categories, 52 products, 36 states, no users/orders",
          q("SELECT (SELECT COUNT(*) FROM categories),(SELECT COUNT(*) FROM products),(SELECT COUNT(*) FROM states),"
            "(SELECT COUNT(*) FROM users),(SELECT COUNT(*) FROM orders)") == "15\t52\t36\t0\t0")
    check("every product has a category and all are active", q("SELECT COUNT(*) FROM products WHERE is_active<>1") == "0")

    def col(table, column):
        return q("SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='ecommerce_db' AND TABLE_NAME='%s' AND COLUMN_NAME='%s'" % (table, column)) == "1"

    def chk(name):
        return q("SELECT CHECK_CLAUSE FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA='ecommerce_db' AND CONSTRAINT_NAME='%s'" % name)

    check("alter_001: user_sessions.end_reason + FORCED allowed (alter_007)", col("user_sessions", "end_reason") and "FORCED" in chk("chk_sessions_end_reason"))
    check("alter_001: idx_sessions_active_expires", q("SELECT COUNT(*) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA='ecommerce_db' AND INDEX_NAME='idx_sessions_active_expires'") == "2")
    check("alter_003: request_logs columns and checks", all(col("request_logs", c) for c in ("log_id", "method", "path", "status", "latency_ms", "queue_wait_ms", "worker_id", "user_id", "created_at"))
          and chk("chk_request_logs_status") != "" and chk("chk_request_logs_times") != "")
    check("alter_004: users.role with CHECK", col("users", "role") and "ADMIN" in chk("chk_users_role"))
    check("alter_005: products.is_active", col("products", "is_active"))
    check("alter_010/011: users.must_change_pw does not exist (the forced password change feature was removed)", not col("users", "must_change_pw"))
    check("alter_006/008: order statuses are the final six", all(s in chk("chk_orders_status") for s in ("PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED"))
          and "PENDING" not in chk("chk_orders_status"), chk("chk_orders_status"))
    check("alter_006: payments statuses", all(s in chk("chk_payments_status") for s in ("PENDING", "PAID", "REFUNDED", "CANCELLED")))
    check("alter_007: admin_actions table", all(col("admin_actions", c) for c in ("action_id", "admin_id", "action", "details", "created_at")))
    check("alter_008: orders.next_step_at + order_status_history", col("orders", "next_step_at") and all(col("order_status_history", c) for c in ("history_id", "order_id", "status", "changed_at", "changed_by"))
          and "SYSTEM" in chk("chk_history_by"))
    check("alter_009: addresses has every column", all(col("addresses", c) for c in ("address_id", "user_id", "first_name", "last_name", "phone", "line1", "line2", "landmark", "city", "state_id",
                                                                                "pincode", "address_type", "is_default", "is_active", "created_at", "default_user")))
    check("alter_009: order_addresses snapshot columns (state_name as text, no FK to addresses)", col("order_addresses", "state_name")
          and q("SELECT COUNT(*) FROM information_schema.KEY_COLUMN_USAGE WHERE TABLE_SCHEMA='ecommerce_db' AND TABLE_NAME='order_addresses' AND REFERENCED_TABLE_NAME IN ('addresses','states')") == "0")
    check("cart: one ACTIVE cart per user (generated column + UNIQUE)", col("cart", "active_user_id"))

    # behaviour of the constraints
    q("INSERT INTO users(email,name,password) VALUES('a@x.in','A','x')")
    ins = ("INSERT INTO addresses(user_id,first_name,last_name,phone,line1,city,state_id,pincode,address_type,is_default) "
           "VALUES(1,'A','B','%s','l','c',1,'%s','%s',%d)")
    bad = lambda *a: t.run(ins % a, db="ecommerce_db", expect_error=True)[0]
    check("good address accepted", not bad("9876543210", "110001", "HOME", 1))
    check("second default for the same user rejected", bad("9876543210", "110001", "HOME", 1))
    check("bad phone / pincode / type rejected", bad("98765", "110001", "HOME", 0) and bad("9876543210", "010001", "HOME", 0) and bad("9876543210", "110001", "CAFE", 0))
    check("negative stock rejected", t.run("UPDATE products SET stock=-1 WHERE product_id=1", db="ecommerce_db", expect_error=True)[0])
    check("bad order status rejected", t.run("INSERT INTO cart(user_id) VALUES(1); INSERT INTO orders(user_id,cart_id,total,status) VALUES(1,1,1,'PENDING')", db="ecommerce_db", expect_error=True)[0])
finally:
    t.stop()
print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
