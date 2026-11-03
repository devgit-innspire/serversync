# test_budget_sql.py - the home page's "Budget picks" query (BUDGET_SQL in src/pages.cpp) and the /products price filter,
# run on a throwaway MySQL (throwaway_mysql.py) with the real 52-product schema. Never reads db.conf, never touches your database.
#     python tests\test_budget_sql.py
import os, re, sys
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

# take the exact SQL text out of pages.cpp: the string literals (and BUDGET_MAX from common.h) between "BUDGET_SQL =" and ";"
src = read(r"src\pages.cpp")
budget_max = int(re.search(r"const int BUDGET_MAX = (\d+);", read(r"include\common.h")).group(1))
block = re.search(r"const string BUDGET_SQL =(.*?);", src, re.S).group(1)
parts = re.findall(r'"((?:[^"\\]|\\.)*)"|(to_string\(BUDGET_MAX\))', block)
BUDGET = "".join(str(budget_max) if p[1] else p[0] for p in parts)
CATEGORIES = re.search(r'const string CATEGORIES_SQL = "(.*?)";', src).group(1)
assert BUDGET.endswith("LIMIT 10") and "p.price <= %d" % budget_max in BUDGET, BUDGET

def rows(q):
    out = db.run(q, "ecommerce_db")
    return [l.split("\t") for l in out.split("\n")] if out else []

try:
    db.start()
    db.run(read(r"sql\ecommerce_db.sql"))

    r = rows(BUDGET)
    check("returns rows, at most 10", 0 < len(r) <= 10, str(len(r)))
    check("each row has the 6 columns the product card uses: id, name, category, price, stock, description", all(len(x) == 6 for x in r))
    check("every price is at most BUDGET_MAX (%d)" % budget_max, all(float(x[3]) <= budget_max for x in r))
    check("every product is in stock", all(int(x[4]) > 0 for x in r))
    check("cheapest first", [float(x[3]) for x in r] == sorted(float(x[3]) for x in r))
    all_cheap = rows("SELECT COUNT(*) FROM products WHERE price <= %d AND stock > 0" % budget_max)[0][0]
    check("nothing cheaper is skipped (10 cheapest of %s in-stock budget products)" % all_cheap, len(r) == min(10, int(all_cheap)))
    check("the category list query returns all 15 categories as (id, name), in id order",
          len(rows(CATEGORIES)) == 15 and all(len(x) == 2 for x in rows(CATEGORIES)) and [int(x[0]) for x in rows(CATEGORIES)] == sorted(int(x[0]) for x in rows(CATEGORIES)))

    db.run("UPDATE products SET stock = 0 WHERE product_id = %s" % r[0][0], "ecommerce_db")
    r2 = rows(BUDGET)
    check("a product that sold out disappears from the budget picks", all(x[0] != r[0][0] for x in r2))
    db.run("UPDATE products SET stock = 0", "ecommerce_db")
    check("nothing in stock: empty list (the page then hides the section)", rows(BUDGET) == [])

    # the /products filter: same SQL pieces as products_page() with ? replaced by the value
    base = ("SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description FROM products p "
            "JOIN categories c ON c.category_id = p.category_id WHERE 1=1")
    check("products_page SQL still has the 6 columns and the price filters", all(s in src for s in ("p.price >= ?", "p.price <= ?", "p.description \"")))
    db.run("UPDATE products SET stock = 5", "ecommerce_db")
    f = rows(base + " AND p.price >= 100 AND p.price <= 500 ORDER BY p.name LIMIT 200")
    check("min 100 / max 500 keeps only products in that range", f and all(100 <= float(x[3]) <= 500 for x in f) and all(len(x) == 6 for x in f), str(len(f)))
    check("min and max can be equal", all(float(x[3]) == float(f[0][3]) for x in rows(base + " AND p.price >= %s AND p.price <= %s" % (f[0][3], f[0][3]))))
finally:
    db.stop()

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
