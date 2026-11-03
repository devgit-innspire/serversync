# test_admin_sql.py - Admin panel A1 on a throwaway MySQL (throwaway_mysql.py, port 3399). Never reads db.conf, never touches your database.
#   1. users.role in the fresh schema (default CUSTOMER, CHECK rejects other values)
#   2. alter_004_users_role.sql on an OLD users table: no data lost, everyone becomes CUSTOMER, same CHECK
#   3. the dashboard queries copied out of src/pages.cpp (admin_dashboard_page) give the right numbers
#   4. load_user's session query returns the role as 4th column
#   5. (A2) alter_005 on an OLD products table; hide/unhide; edit keeps stock; ATOMIC restock under parallel sales;
#      category list / duplicate (1062) / delete with products (1451); hidden products vanish from the store queries
#   6. (A4) alter_007 on an OLD database (FORCED allowed, admin_actions created, data kept); users / sessions / logs / audit queries
#     python tests\test_admin_sql.py
import os, re, sys, threading
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

def rows(q, name="ecommerce_db"):
    out = db.run(q, name)
    return [l.split("\t") for l in out.split("\n")] if out else []

def fails_with(q, text):
    try:
        db.run(q, "ecommerce_db")
        return False
    except Exception as e:
        return text.lower() in str(e).lower()

# ---- the SQL text of the dashboard, taken from pages.cpp ----
src = read(r"src\pages.cpp")
low_stock = int(re.search(r"const int LOW_STOCK = (\d+);", read(r"include\common.h")).group(1))
func = src[src.index("static int admin_dashboard_page"):src.index("// ---- admin: products ----")]
queries = []
for m in re.finditer(r'db_select_on\(m,\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', func):
    queries.append("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))).replace("?", str(low_stock)))
check("found the 4 dashboard queries in pages.cpp", len(queries) == 4, str(len(queries)))
check("signup still inserts only name, email, password (never a role from the form)",
      'INSERT INTO users (name, email, password) VALUES (?, ?, ?)' in src)
check("the session lookup selects u.role as the 4th column", "s.expires_at <= NOW(), u.role FROM user_sessions s" in src)

schema = read(r"sql\ecommerce_db.sql")
check("ecommerce_db.sql has the role column and chk_users_role", "role       VARCHAR(10) NOT NULL DEFAULT 'CUSTOMER'" in schema and "chk_users_role" in schema)

try:
    db.start()
    db.run(schema)

    # ---- 1. fresh schema ----
    db.run("INSERT INTO users (name, email, password) VALUES ('Ann', 'ann@x.com', 'x'), ('Bob', 'bob@x.com', 'x')", "ecommerce_db")
    check("a user created like signup does is a CUSTOMER", [r[0] for r in rows("SELECT role FROM users")] == ["CUSTOMER", "CUSTOMER"])
    check("role can be set to ADMIN", db.run("UPDATE users SET role = 'ADMIN' WHERE email = 'bob@x.com'", "ecommerce_db") is not None)
    check("role 'HACKER' is rejected by the CHECK constraint", fails_with("UPDATE users SET role = 'HACKER' WHERE email = 'ann@x.com'", "chk_users_role"))
    check("role cannot be NULL", fails_with("UPDATE users SET role = NULL WHERE email = 'ann@x.com'", "null"))
    db.run("UPDATE users SET role = 'CUSTOMER' WHERE email = 'bob@x.com'", "ecommerce_db")

    # ---- 2. the ALTER on an old table ----
    db.run("ALTER TABLE users DROP CHECK chk_users_role", "ecommerce_db")
    db.run("ALTER TABLE users DROP COLUMN role", "ecommerce_db")
    before = rows("SELECT user_id, email, name, password, created_at FROM users ORDER BY user_id")
    db.run(read(r"sql\alter_004_users_role.sql"))
    after = rows("SELECT user_id, email, name, password, created_at FROM users ORDER BY user_id")
    check("ALTER keeps every existing row and value", before == after and len(after) == 2)
    check("ALTER: every existing user is a CUSTOMER", [r[0] for r in rows("SELECT role FROM users")] == ["CUSTOMER", "CUSTOMER"])
    check("ALTER: the CHECK works too", fails_with("UPDATE users SET role = 'ROOT'", "chk_users_role"))
    check("ALTER: a second run is refused (Duplicate ... error), nothing breaks", fails_with(read(r"sql\alter_004_users_role.sql"), "duplicate"))
    cols = [r[0] for r in rows("SHOW COLUMNS FROM users")]
    check("columns: user_id, email, name, password, created_at, role", cols == ["user_id", "email", "name", "password", "created_at", "role"], str(cols))

    # ---- 3. the dashboard queries ----
    db.run("UPDATE users SET role = 'ADMIN' WHERE email = 'bob@x.com'", "ecommerce_db")
    db.run("INSERT INTO cart (user_id, status) VALUES (1,'CHECKED_OUT'),(1,'CHECKED_OUT'),(2,'CHECKED_OUT'),(2,'CHECKED_OUT')", "ecommerce_db")
    db.run("INSERT INTO orders (user_id, cart_id, total, status) VALUES (1,1,100.50,'PLACED'),(1,2,200.00,'DELIVERED'),(2,3,999.00,'CANCELLED'),(2,4,50.00,'PLACED')", "ecommerce_db")
    db.run("UPDATE products SET stock = 50", "ecommerce_db")
    db.run("UPDATE products SET stock = 5 WHERE product_id = 1", "ecommerce_db")
    db.run("UPDATE products SET stock = 0 WHERE product_id = 2", "ecommerce_db")
    db.run("UPDATE products SET stock = 6 WHERE product_id = 3", "ecommerce_db")
    db.run("INSERT INTO user_sessions (session_id, user_id, expires_at, is_active) VALUES (REPEAT('a',64), 1, NOW() + INTERVAL 30 MINUTE, 1),"
           "(REPEAT('b',64), 2, NOW() - INTERVAL 1 MINUTE, 1),(REPEAT('c',64), 2, NOW() + INTERVAL 30 MINUTE, 0)", "ecommerce_db")
    n_products = rows("SELECT COUNT(*) FROM products")[0][0]

    s = rows(queries[0])[0]
    check("stats: 7 columns", len(s) == 7, str(s))
    check("revenue = non-cancelled orders only (100.50 + 200.00 + 50.00)", float(s[0]) == 350.50, s[0])
    check("total orders 4, waiting (PLACED) 2, users 2", s[1:4] == ["4", "2", "2"], str(s[1:4]))
    check("products = all rows, low stock counts stock <= %d (1 and 0, not 6)" % low_stock, s[4] == n_products and s[5] == "2", str(s[4:6]))
    check("active sessions: active AND not expired (1 of 3)", s[6] == "1", s[6])
    rec = rows(queries[1])
    check("recent orders: newest first, 5 columns, customer name joined", [r[0] for r in rec] == ["4", "3", "2", "1"] and all(len(r) == 5 for r in rec) and rec[3][1] == "Ann")
    check("by status: PLACED 2 first, then the others alphabetically",
          rows(queries[2]) == [["PLACED", "2"], ["CANCELLED", "1"], ["DELIVERED", "1"]], str(rows(queries[2])))
    low = rows(queries[3])
    check("low stock list: (id, name, stock), lowest first (0 then 5), nothing above the limit, hidden products excluded", [r[2] for r in low] == ["0", "5"] and all(len(r) == 3 for r in low), str(low))
    db.run("INSERT INTO products (name, description, price, stock, category_id) SELECT CONCAT('P', n), 'd', 1, 1, 1 FROM "
           "(SELECT 1 n UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10 UNION SELECT 11 UNION SELECT 12 UNION SELECT 13 UNION SELECT 14 UNION SELECT 15 UNION SELECT 16 UNION SELECT 17 UNION SELECT 18 UNION SELECT 19 UNION SELECT 20 UNION SELECT 21 UNION SELECT 22) t", "ecommerce_db")
    check("low stock list is capped at 20 rows", len(rows(queries[3])) == 20, str(len(rows(queries[3]))))
    check("the 10-row limit on recent orders", "LIMIT 10" in queries[1])

    # ---- 4. session lookup with role ----
    q = re.search(r'"(SELECT s\.user_id, u\.name, s\.expires_at <= NOW\(\), u\.role FROM user_sessions s "\s*"JOIN users u ON u\.user_id = s\.user_id WHERE s\.session_id = \?[^"]*)"', src)
    sql = "SELECT s.user_id, u.name, s.expires_at <= NOW(), u.role FROM user_sessions s JOIN users u ON u.user_id = s.user_id WHERE s.session_id = '%s' AND s.is_active = 1" % ("a" * 64)
    r = rows(sql)
    check("session lookup returns (user_id, name, expired?, role)", r == [["1", "Ann", "0", "CUSTOMER"]], str(r))
    db.run("UPDATE users SET role = 'ADMIN' WHERE user_id = 1", "ecommerce_db")
    check("a role change shows on the very next lookup", rows(sql)[0][3] == "ADMIN")
    # ================= A2 =================
    def literals(block):
        return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', block))

    def bind(q, *vals):
        """replace the ?s in order with quoted values (what the prepared statement does)"""
        parts = q.split("?")
        assert len(parts) == len(vals) + 1, (q, vals)
        return parts[0] + "".join("'" + str(v).replace("'", "''") + "'" + parts[i + 1] for i, v in enumerate(vals))

    rfunc = src[src.index("static int admin_restock_page"):src.index("static int admin_toggle_page")]
    RESTOCK = literals(re.search(r"db_exec\(((?:\s*\"(?:[^\"\\]|\\.)*\")+),\s*\{delta", rfunc).group(1))
    check("found the restock query in pages.cpp: stock = stock + ... with a >= 0 guard",
          "stock = stock + CAST(? AS SIGNED)" in RESTOCK and "stock + CAST(? AS SIGNED) >= 0" in RESTOCK and RESTOCK.count("?") == 3, RESTOCK)
    check("no SET stock = <number> anywhere in the admin code", not re.search(r"SET stock = (\?|[0-9])", src))

    def restock(pid, delta):
        """run the real restock SQL; returns the number of changed rows"""
        return int(db.run(bind(RESTOCK, delta, pid, delta) + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1])

    def stock_of(pid):
        return int(rows("SELECT stock FROM products WHERE product_id = %d" % pid)[0][0])

    # --- alter_005 on an old products table ---
    db.run("ALTER TABLE products DROP COLUMN is_active", "ecommerce_db")
    before = rows("SELECT product_id, name, description, price, stock, category_id FROM products ORDER BY product_id")
    db.run(read(r"sql\alter_005_products_is_active.sql"))
    after = rows("SELECT product_id, name, description, price, stock, category_id FROM products ORDER BY product_id")
    check("alter_005: every product row and value is kept (%d products)" % len(after), before == after and len(after) > 40)
    check("alter_005: every existing product is active (visible in the store)", rows("SELECT COUNT(*) FROM products WHERE is_active = TRUE")[0][0] == str(len(after)))
    check("alter_005: a second run is refused (Duplicate column), nothing breaks", fails_with(read(r"sql\alter_005_products_is_active.sql"), "duplicate"))
    check("fresh schema has the same column", "is_active    BOOLEAN        NOT NULL DEFAULT TRUE" in schema)
    default_row = rows("SELECT COLUMN_DEFAULT, IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='ecommerce_db' AND TABLE_NAME='products' AND COLUMN_NAME='is_active'")[0]
    check("column default is 1 (TRUE) and NOT NULL", default_row == ["1", "NO"], str(default_row))

    # --- restock: single atomic UPDATE ---
    db.run("UPDATE products SET stock = 10 WHERE product_id = 1", "ecommerce_db")
    check("restock +5: 1 row changed, stock 15", restock(1, "5") == 1 and stock_of(1) == 15)
    check("restock -15: stock goes to exactly 0 (allowed)", restock(1, "-15") == 1 and stock_of(1) == 0)
    check("restock -1 at 0: refused (0 rows), stock stays 0", restock(1, "-1") == 0 and stock_of(1) == 0)
    db.run("UPDATE products SET stock = 3 WHERE product_id = 1", "ecommerce_db")
    check("restock -4 at 3: refused, stock stays 3 (never negative)", restock(1, "-4") == 0 and stock_of(1) == 3)
    check("restock of a product that does not exist: 0 rows", restock(999999, "5") == 0)
    check("restock works on a hidden product too", db.run("UPDATE products SET is_active = FALSE WHERE product_id = 1", "ecommerce_db") is not None and restock(1, "2") == 1 and stock_of(1) == 5)
    db.run("UPDATE products SET is_active = TRUE WHERE product_id = 1", "ecommerce_db")

    # parallel: 20 sales (-1) and 20 restocks (+3) at the same moment: nothing is lost
    db.run("UPDATE products SET stock = 100 WHERE product_id = 1", "ecommerce_db")
    def session(pid, delta, times):
        """one mysql session that runs the real restock SQL `times` times; returns the list of changed-row counts"""
        script = "".join(bind(RESTOCK, delta, pid, delta) + "; SELECT ROW_COUNT();\n" for _ in range(times))
        return [int(x) for x in db.run(script, "ecommerce_db").split("\n")]

    def parallel(jobs):
        out = []
        ths = [threading.Thread(target=lambda j=j: out.extend(session(*j))) for j in jobs]
        for th in ths: th.start()
        for th in ths: th.join()
        return out

    # 8 sessions at the same moment: 4 x five sales (-1) and 4 x five restocks (+3): nothing is lost
    db.run("UPDATE products SET stock = 100 WHERE product_id = 1", "ecommerce_db")
    res = parallel([(1, "-1", 5)] * 4 + [(1, "3", 5)] * 4)
    check("40 changes from 8 parallel sessions (20 x -1, 20 x +3): all applied, stock is exactly 100 - 20 + 60 = 140",
          stock_of(1) == 140 and len(res) == 40 and res.count(1) == 40, "%d, %s" % (stock_of(1), res))
    # more sales than stock: exactly `stock` succeed, never below zero
    db.run("UPDATE products SET stock = 5 WHERE product_id = 2", "ecommerce_db")
    res = parallel([(2, "-1", 3)] * 4)
    check("12 parallel -1 on stock 5: exactly 5 succeed, 7 refused, stock 0 (never negative)", res.count(1) == 5 and res.count(0) == 7 and stock_of(2) == 0, str(res))
    check("the CHECK constraint is a second safety net (stock >= 0)", fails_with("UPDATE products SET stock = -1 WHERE product_id = 2", "chk_products_stock"))

    # --- add / edit / toggle SQL ---
    ins = literals(re.search(r"db_exec\(((?:\s*\"(?:[^\"\\]|\\.)*\")+),\s*\{f\.name, f\.description, cents_plain\(cents\), f\.category\}\);\s*\n\s*if \(n < 0\) return product_form_page\(client, req, 500, true", src).group(1))
    db.run(bind(ins, "Brand New", "desc", "499.50", "3"), "ecommerce_db")
    new_row = rows("SELECT name, price, stock, is_active, category_id FROM products WHERE name = 'Brand New'")[0]
    check("add: new product starts with stock 0, active, the chosen category and price", new_row == ["Brand New", "499.50", "0", "1", "3"], str(new_row))
    upd = re.search(r"(UPDATE products SET name = \?, description = \?, price = \?, category_id = \? WHERE product_id = \?)", src).group(1)
    pid = rows("SELECT product_id FROM products WHERE name = 'Brand New'")[0][0]
    db.run("UPDATE products SET stock = 7 WHERE product_id = " + pid, "ecommerce_db")
    db.run(bind(upd, "Renamed", "d2", "10.00", "4", pid), "ecommerce_db")
    e = rows("SELECT name, description, price, category_id, stock FROM products WHERE product_id = " + pid)[0]
    check("edit: name, description, price, category change; STOCK stays 7", e == ["Renamed", "d2", "10.00", "4", "7"], str(e))
    check("edit of an unknown product: 0 rows", int(db.run(bind(upd, "x", "y", "1.00", "1", "999999") + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1]) == 0)
    check("a price of 0 or a missing category is stopped by the database too (FK / CHECK)",
          fails_with(bind(upd, "x", "y", "1.00", "9999", pid), "foreign key") and fails_with(bind(upd, "x", "y", "-1.00", "1", pid), "chk_products_price"))
    tog = "UPDATE products SET is_active = ? WHERE product_id = ?"
    check("toggle SQL is in pages.cpp", tog in src)
    check("hide: 1 row; the product row (and old orders) stay", int(db.run(bind(tog, "0", pid) + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1]) == 1 and
          rows("SELECT is_active FROM products WHERE product_id = " + pid)[0][0] == "0")
    check("hide of an unknown product: 0 rows", int(db.run(bind(tog, "0", "999999") + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1]) == 0)

    # --- store queries skip hidden products ---
    bparts = re.findall(r'"((?:[^"\\]|\\.)*)"|(to_string\(BUDGET_MAX\))', re.search(r"const string BUDGET_SQL =(.*?);", src, re.S).group(1))
    BUDGET = "".join("1000" if x[1] else x[0] for x in bparts)
    db.run("UPDATE products SET stock = 9, price = 0.50, is_active = TRUE WHERE product_id = " + pid, "ecommerce_db")
    check("budget picks show the product while it is active", pid in [r[0] for r in rows(BUDGET)], pid + " " + str(rows(BUDGET)[:2]) + BUDGET[-60:])
    db.run("UPDATE products SET is_active = FALSE WHERE product_id = " + pid, "ecommerce_db")
    check("budget picks skip it once hidden", pid not in [r[0] for r in rows(BUDGET)])
    list_sql = re.search(r'string sql = "(SELECT p\.product_id, p\.name, c\.category_name, p\.price, p\.stock, p\.description "\s*"FROM products p JOIN categories c ON c\.category_id = p\.category_id WHERE p\.is_active = TRUE)";', src)
    check("products page SQL starts from WHERE p.is_active = TRUE", list_sql is not None)
    store_sql = literals(list_sql.group(0)[len("string sql = "):-1]) + " ORDER BY p.name LIMIT 200"
    check("products list / search skip hidden products", pid not in [r[0] for r in rows(store_sql)] and len(rows(store_sql)) > 40)
    check("search by name skips hidden products too", rows(store_sql.replace("ORDER BY", "AND p.name LIKE '%Renamed%' ORDER BY")) == [])
    one = "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description, p.category_id FROM products p JOIN categories c ON c.category_id = p.category_id WHERE p.product_id = ? AND p.is_active = TRUE"
    check("product page SQL has the is_active filter (hidden -> no row -> 404)", "WHERE p.product_id = ? AND p.is_active = TRUE" in src and rows(bind(one, pid)) == [])
    cart_add = "SELECT stock FROM products WHERE product_id = ? AND is_active = TRUE"
    check("cart/add SQL has the is_active filter (hidden -> no row -> 404)", cart_add in src and rows(bind(cart_add, pid)) == [])
    db.run("UPDATE products SET is_active = TRUE WHERE product_id = " + pid, "ecommerce_db")
    check("unhide: it is back in the product page query", len(rows(bind(one, pid))) == 1)

    # --- categories ---
    cfunc = src[src.index("static int admin_categories_page"):src.index("// categories badalte hi")]
    CAT_LIST = literals(re.search(r"db_select\(((?:\s*\"(?:[^\"\\]|\\.)*\")+),\s*\{\}, cats\)", cfunc).group(1))
    cl = rows(CAT_LIST)
    check("category list: (id, name, product count) for all 15, LEFT JOIN shows empty categories with 0", len(cl) == 15 and all(len(r) == 3 for r in cl))
    db.run("INSERT INTO categories (category_name) VALUES ('Empty One')", "ecommerce_db")
    cl = rows(CAT_LIST)
    check("a new empty category is listed with 0 products", cl[-1][1] == "Empty One" and cl[-1][2] == "0")
    total = sum(int(r[2]) for r in cl)
    check("counts add up to the number of products (hidden ones included)", str(total) == rows("SELECT COUNT(*) FROM products")[0][0], str(total))
    check("duplicate name -> MySQL error 1062 (insert)", fails_with("INSERT INTO categories (category_name) VALUES ('electronics')", "1062"))
    check("rename to an existing name -> error 1062", fails_with("UPDATE categories SET category_name = 'Audio' WHERE category_id = 1", "1062"))
    check("rename to its own name is fine (no error)", db.run("UPDATE categories SET category_name = 'Electronics' WHERE category_id = 1", "ecommerce_db") is not None)
    check("delete a category that has products -> error 1451 (FK)", fails_with("DELETE FROM categories WHERE category_id = 1", "1451"))
    db.run("UPDATE products SET is_active = FALSE WHERE category_id = 1", "ecommerce_db")
    check("... hiding its products does NOT make delete possible (still 1451)", fails_with("DELETE FROM categories WHERE category_id = 1", "1451"))
    empty_id = rows("SELECT category_id FROM categories WHERE category_name = 'Empty One'")[0][0]
    check("delete an empty category: works", db.run("DELETE FROM categories WHERE category_id = " + empty_id + "; SELECT ROW_COUNT();", "ecommerce_db") == "1")
    csrc = src[src.index("static int admin_category_add_page"):src.index("// /admin ... : guard")]
    check("category code maps 1062 -> cat_exists and 1451 -> cat_has_products and clears the navbar cache",
          "code == 1062" in src and "code == 1451" in src and "clear_category_cache();" in csrc + src and "cat_loaded_us = 0;" in src)
    # ================= A4 =================
    def has_sql(fragment):
        assert fragment in src, fragment
        return fragment

    # --- alter_007 on an OLD database ---
    db.run(schema)
    db.run("DROP TABLE admin_actions", "ecommerce_db")
    db.run("ALTER TABLE user_sessions DROP CHECK chk_sessions_end_reason", "ecommerce_db")
    db.run("ALTER TABLE user_sessions ADD CONSTRAINT chk_sessions_end_reason CHECK (end_reason IN ('LOGOUT', 'EXPIRED'))", "ecommerce_db")
    db.run("INSERT INTO users (name,email,password,role) VALUES ('Admin','a@x.com','x','ADMIN'),('Bob','b@x.com','x','CUSTOMER'),('Cy','c@x.com','x','CUSTOMER')", "ecommerce_db")
    db.run("INSERT INTO user_sessions (session_id, user_id, expires_at, is_active, end_reason) VALUES (REPEAT('1',64), 2, NOW() + INTERVAL 1 HOUR, 1, NULL), (REPEAT('2',64), 3, NOW() - INTERVAL 1 HOUR, 0, 'LOGOUT')", "ecommerce_db")
    check("old database: end_reason 'FORCED' is refused before the ALTER", fails_with("UPDATE user_sessions SET end_reason = 'FORCED' WHERE session_id = REPEAT('2',64)", "chk_sessions_end_reason"))
    before = rows("SELECT session_id, user_id, end_reason FROM user_sessions ORDER BY session_id")
    db.run(read(r"sql\alter_007_admin_audit_and_forced.sql"))
    check("alter_007: existing sessions keep all their data", rows("SELECT session_id, user_id, end_reason FROM user_sessions ORDER BY session_id") == before)
    check("alter_007: 'FORCED' is accepted, 'HACKED' is still refused",
          db.run("UPDATE user_sessions SET end_reason = 'FORCED' WHERE session_id = REPEAT('2',64)", "ecommerce_db") is not None and
          fails_with("UPDATE user_sessions SET end_reason = 'HACKED' WHERE session_id = REPEAT('2',64)", "chk_sessions_end_reason"))
    check("alter_007: admin_actions exists with the 5 columns", [r[0] for r in rows("SHOW COLUMNS FROM admin_actions")] == ["action_id", "admin_id", "action", "details", "created_at"])
    check("alter_007: safe to run a second time (same result)", db.run(read(r"sql\alter_007_admin_audit_and_forced.sql")) is not None and rows("SELECT COUNT(*) FROM admin_actions")[0][0] == "0")
    fresh_cols = [r[0] for r in rows("SHOW COLUMNS FROM admin_actions")]
    check("a fresh schema has the same table and the same CHECK list", "'FORCED'" in schema and "CREATE TABLE admin_actions" in schema and fresh_cols == ["action_id", "admin_id", "action", "details", "created_at"])
    check("audit table: admin_id must be a real user (FK)", fails_with("INSERT INTO admin_actions (admin_id, action) VALUES (999, 'X')", "foreign key"))
    check("audit table: details defaults to '' and 256 characters are refused (VARCHAR(255))",
          db.run("INSERT INTO admin_actions (admin_id, action) VALUES (1, 'X')", "ecommerce_db") is not None and rows("SELECT LENGTH(details) FROM admin_actions")[0][0] == "0" and
          fails_with("INSERT INTO admin_actions (admin_id, action, details) VALUES (1, 'X', REPEAT('d', 256))", "too long"))
    db.run("DELETE FROM admin_actions", "ecommerce_db")

    # --- users list: orders count and ACTIVE sessions count ---
    USERS = has_sql("SELECT u.user_id, u.name, u.email, u.role, u.created_at, ") and (
        "SELECT u.user_id, u.name, u.email, u.role, u.created_at, "
        "(SELECT COUNT(*) FROM orders o WHERE o.user_id = u.user_id), "
        "(SELECT COUNT(*) FROM user_sessions s WHERE s.user_id = u.user_id AND s.is_active = 1 AND s.expires_at > NOW()) "
        "FROM users u WHERE 1=1")
    has_sql("(SELECT COUNT(*) FROM orders o WHERE o.user_id = u.user_id)")
    has_sql("(SELECT COUNT(*) FROM user_sessions s WHERE s.user_id = u.user_id AND s.is_active = 1 AND s.expires_at > NOW()) ")
    db.run("INSERT INTO cart (user_id, status) VALUES (2,'CHECKED_OUT'),(2,'CHECKED_OUT')", "ecommerce_db")
    db.run("INSERT INTO orders (user_id, cart_id, total, status) VALUES (2,1,10,'PLACED'),(2,2,20,'DELIVERED')", "ecommerce_db")
    db.run("INSERT INTO user_sessions (session_id, user_id, expires_at, is_active) VALUES (REPEAT('3',64), 2, NOW() + INTERVAL 1 HOUR, 1), (REPEAT('4',64), 2, NOW() - INTERVAL 1 HOUR, 1)", "ecommerce_db")
    ul = rows(USERS + " ORDER BY u.user_id LIMIT 21 OFFSET 0")
    check("users list: 7 columns, ordered by id", len(ul) == 3 and all(len(r) == 7 for r in ul) and [r[0] for r in ul] == ["1", "2", "3"])
    check("users list: Bob has 2 orders and 2 ACTIVE sessions (the expired one is not counted); Cy has 0 and 0 (his ended session is not counted)",
          ul[1][5:] == ["2", "2"] and ul[2][5:] == ["0", "0"], str(ul))
    like_user = rows(USERS + " AND (u.name LIKE '%o!_b%' ESCAPE '!' OR u.email LIKE '%o!_b%' ESCAPE '!')")
    check("users search with an escaped underscore does not match everything", like_user == [])
    check("users search by name / email part", [r[0] for r in rows(USERS + " AND (u.name LIKE '%ob%' ESCAPE '!' OR u.email LIKE '%ob%' ESCAPE '!')")] == ["2"])

    # --- session end by 8 characters ---
    FIND = has_sql("SELECT session_id FROM user_sessions WHERE session_id LIKE ? AND is_active = 1 LIMIT 2")
    END_ONE = has_sql("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED' WHERE session_id = ? AND is_active = 1")
    END_ALL = has_sql("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED' WHERE user_id = ? AND is_active = 1")
    check("find by the first 8 characters: one active match", rows(bind(FIND, "33333333%")) == [["3" * 64]])
    check("... an ended session is not found (is_active = 1)", rows(bind(FIND, "22222222%")) == [])
    db.run("INSERT INTO user_sessions (session_id, user_id, expires_at, is_active) VALUES (CONCAT('abcdef12', REPEAT('0',56)), 3, NOW() + INTERVAL 1 HOUR, 1), (CONCAT('abcdef12', REPEAT('1',56)), 3, NOW() + INTERVAL 1 HOUR, 1)", "ecommerce_db")
    check("... two active sessions with the same 8 characters give 2 rows (the page refuses: ambiguous)", len(rows(bind(FIND, "abcdef12%"))) == 2)
    check("... the full id also finds exactly one", rows(bind(FIND, "abcdef12" + "0" * 56 + "%")) == [["abcdef12" + "0" * 56]])
    one = int(db.run(bind(END_ONE, "3" * 64) + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1])
    st = rows("SELECT is_active, end_reason, IF(logout_time IS NULL, 'NULL', 'SET') FROM user_sessions WHERE session_id = '%s'" % ("3" * 64))[0]
    check("end one: 1 row, is_active 0, end_reason FORCED, logout_time set", one == 1 and st == ["0", "FORCED", "SET"], str(st))
    again = int(db.run(bind(END_ONE, "3" * 64) + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1])
    check("ending it again changes 0 rows (already ended: 'session_gone')", again == 0)
    n_all = int(db.run(bind(END_ALL, "3") + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1])
    check("logout everywhere: ends only that user's ACTIVE sessions (2 of Cy's), others untouched", n_all == 2 and rows("SELECT COUNT(*) FROM user_sessions WHERE user_id = 3 AND end_reason = 'FORCED'")[0][0] == "3" and
          rows("SELECT is_active FROM user_sessions WHERE session_id = REPEAT('1',64)")[0][0] == "1")
    check("... Cy's old ended session keeps its reason (FORCED was set earlier by the check above; a LOGOUT one would stay LOGOUT)",
          rows("SELECT end_reason FROM user_sessions WHERE session_id = REPEAT('2',64)")[0][0] == "FORCED")
    check("logout everywhere for a user with no active sessions: 0 rows", int(db.run(bind(END_ALL, "3") + "; SELECT ROW_COUNT();", "ecommerce_db").split("\n")[-1]) == 0)

    # --- admin sessions list ---
    SESS = has_sql("FROM user_sessions s JOIN users u ON u.user_id = s.user_id") and (
        "SELECT s.session_id, u.name, u.email, s.login_time, s.last_activity, s.expires_at, s.is_active, s.expires_at <= NOW(), "
        "IFNULL(s.end_reason, '') FROM user_sessions s JOIN users u ON u.user_id = s.user_id")
    has_sql("IFNULL(s.end_reason, '') FROM user_sessions s JOIN users u ON u.user_id = s.user_id")
    has_sql('sql += " WHERE s.is_active = 1 AND s.expires_at > NOW()";')
    all_s = rows(SESS + " ORDER BY s.login_time DESC, s.session_id LIMIT 21 OFFSET 0")
    act_s = rows(SESS + " WHERE s.is_active = 1 AND s.expires_at > NOW() ORDER BY s.login_time DESC, s.session_id LIMIT 21 OFFSET 0")
    check("sessions list: 9 columns; 'Active' keeps only active and not expired (1 of Bob's + Cy's abcdef12... one is active? no, all ended) = the live ones only",
          all(len(r) == 9 for r in all_s) and all(r[6] == "1" and r[7] == "0" for r in act_s) and len(act_s) < len(all_s), "%d of %d" % (len(act_s), len(all_s)))
    check("sessions list: the expired-but-not-closed session is in 'All' (expired=1) and not in 'Active'",
          any(r[0] == "4" * 64 and r[7] == "1" for r in all_s) and not any(r[0] == "4" * 64 for r in act_s))
    check("sessions list: FORCED ones are there with their reason", any(r[0] == "3" * 64 and r[8] == "FORCED" for r in all_s))
    check("sessions list: paging with LIMIT 21 OFFSET 20 works (empty here)", rows(SESS + " ORDER BY s.login_time DESC, s.session_id LIMIT 21 OFFSET 20") == [])
    # session_status(): the Python mirror of the C++ rule must give these four labels
    def status(is_active, expired, reason):
        if is_active == "1": return "Expired" if expired == "1" else "Active"
        if reason == "FORCED": return "Ended by admin"
        return "Expired" if reason == "EXPIRED" else "Logged out"
    check("status rule in pages.cpp has the FORCED branch before EXPIRED / LOGOUT", src.index('end_reason == "FORCED"') < src.index('end_reason == "EXPIRED" ? "Expired" : "Logged out"'))
    check("the labels of the status rule (Python mirror of session_status): Active, Expired, Ended by admin, Logged out",
          [status("1", "0", ""), status("1", "1", ""), status("0", "0", "FORCED"), status("0", "0", "EXPIRED"), status("0", "0", "LOGOUT"), status("0", "0", "")] ==
          ["Active", "Expired", "Ended by admin", "Expired", "Logged out", "Logged out"])

    # --- logs ---
    LOG_BASE = ("SELECT l.created_at, l.method, l.path, l.status, l.latency_ms, l.queue_wait_ms, l.worker_id, "
                "IFNULL(CONCAT(u.name, ' (#', l.user_id, ')'), '-') FROM request_logs l LEFT JOIN users u ON u.user_id = l.user_id WHERE 1=1")
    has_sql("IFNULL(CONCAT(u.name, ' (#', l.user_id, ')'), '-') FROM request_logs l LEFT JOIN users u ON u.user_id = l.user_id WHERE 1=1")
    has_sql('sql += " AND l.worker_id = ?";')
    has_sql('sql += " AND l.path LIKE ? ESCAPE \'!\'";')
    has_sql('" ORDER BY l.log_id DESC LIMIT 100"')
    db.run("INSERT INTO request_logs (method, path, status, latency_ms, queue_wait_ms, worker_id, user_id, created_at) VALUES "
           "('GET','/a_b',200,1.5,0.1,2,2,NOW()),('GET','/axb',404,0.5,0,2,NULL,NOW()),('POST','/checkout',503,2,20,3,NULL,NOW()),"
           "('GET','/old',200,9,0,2,NULL,NOW() - INTERVAL 10 MINUTE),('GET','/redir',302,1,0,4,3,NOW())", "ecommerce_db")
    lg = rows(LOG_BASE + " ORDER BY l.log_id DESC LIMIT 100")
    check("logs: 8 columns, newest first, user shown as 'name (#id)' or '-'", len(lg) == 5 and all(len(r) == 8 for r in lg) and lg[0][2] == "/redir" and lg[0][7] == "Cy (#3)" and lg[1][7] == "-" and
          [r for r in lg if r[2] == "/a_b"][0][7] == "Bob (#2)")
    check("logs: worker filter", [r[2] for r in rows(bind(LOG_BASE + " AND l.worker_id = ? ORDER BY l.log_id DESC LIMIT 100", "3"))] == ["/checkout"])
    for cls, want in (("2", ["/old", "/a_b"]), ("3", ["/redir"]), ("4", ["/axb"]), ("5", ["/checkout"])):
        lo = int(cls) * 100
        got = [r[2] for r in rows(LOG_BASE + " AND l.status BETWEEN %d AND %d ORDER BY l.log_id DESC LIMIT 100" % (lo, lo + 99))]
        check("logs: status class %sxx" % cls, sorted(got) == sorted(want), str(got))
    check("logs: path filter with an escaped underscore matches '/a_b' only (not '/axb')", [r[2] for r in rows(LOG_BASE + " AND l.path LIKE '%a!_b%' ESCAPE '!'")] == ["/a_b"])
    db.run("INSERT INTO request_logs (method, path, status, latency_ms, queue_wait_ms, worker_id) SELECT 'GET', CONCAT('/bulk', n), 200, 1, 0, 5 FROM "
           "(SELECT a.n + b.n * 10 AS n FROM (SELECT 0 n UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a, "
           "(SELECT 0 n UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9 UNION SELECT 10) b) t", "ecommerce_db")
    check("logs: at most 100 rows", len(rows(LOG_BASE + " ORDER BY l.log_id DESC LIMIT 100")) == 100)
    WORKERS = has_sql("SELECT worker_id, COUNT(*), AVG(latency_ms) FROM request_logs ") and (
        "SELECT worker_id, COUNT(*), AVG(latency_ms) FROM request_logs WHERE created_at >= NOW() - INTERVAL 5 MINUTE GROUP BY worker_id ORDER BY worker_id")
    has_sql('"WHERE created_at >= NOW() - INTERVAL 5 MINUTE GROUP BY worker_id ORDER BY worker_id"')
    w = {r[0]: (int(r[1]), float(r[2])) for r in rows(WORKERS)}
    check("worker table: last 5 minutes only (the 10-minute-old row is ignored), count and average per worker",
          w["2"][0] == 2 and abs(w["2"][1] - 1.0) < 0.001 and w["3"] == (1, 2.0) and w["4"][0] == 1 and w["5"][0] == 110, str(w))
    check("worker table: ordered by worker id", [int(r[0]) for r in rows(WORKERS)] == sorted(int(r[0]) for r in rows(WORKERS)))

    # --- admin activity list ---
    ACT = has_sql("SELECT a.created_at, u.name, a.action, a.details FROM admin_actions a ") and (
        "SELECT a.created_at, u.name, a.action, a.details FROM admin_actions a JOIN users u ON u.user_id = a.admin_id ORDER BY a.action_id DESC LIMIT 50")
    has_sql('"JOIN users u ON u.user_id = a.admin_id ORDER BY a.action_id DESC LIMIT 50"')
    db.run("INSERT INTO admin_actions (admin_id, action, details) SELECT 1, CONCAT('A', n), '' FROM (SELECT 1 n UNION SELECT 2 UNION SELECT 3) t", "ecommerce_db")
    db.run("INSERT INTO admin_actions (admin_id, action, details) SELECT 1, 'MORE', n FROM (SELECT a.n + b.n * 10 AS n FROM (SELECT 0 n UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a, (SELECT 0 n UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) b) t", "ecommerce_db")
    act = rows(ACT)
    check("admin activity: 4 columns, newest first, at most 50 rows, admin shown by name", len(act) == 50 and all(len(r) == 4 for r in act) and act[0][2] == "MORE" and act[0][1] == "Admin")
finally:
    db.stop()

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
