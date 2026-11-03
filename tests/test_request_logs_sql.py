# test_request_logs_sql.py - the request_logs table and the real multi-row insert (db_insert_request_logs in src/db.cpp)
# on a throwaway MySQL (throwaway_mysql.py). Never reads or writes db.conf, never touches your real database.
#     python tests\test_request_logs_sql.py
import os, re, shutil, subprocess, sys, tempfile
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

def sql(q, **kw):
    return db.run(q, "ecommerce_db", **kw)

schema = read(r"sql\ecommerce_db.sql")
alter = read(r"sql\alter_003_request_logs.sql")
work = tempfile.mkdtemp(prefix="serversync_rl_")
try:
    exe = os.path.join(work, "request_logs_check.exe")
    r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "request_logs_check.cpp"),
                        "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(db.BASE, "include"),
                        "-L" + os.path.join(db.BASE, "lib"), "-lmysql", "-lws2_32", "-static-libgcc", "-static-libstdc++"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[:2000]
    shutil.copy(os.path.join(ROOT, "libmysql.dll"), work)

    def insert(n, bad=False):
        p = subprocess.run([exe, str(n)] + (["bad"] if bad else []), capture_output=True, text=True, cwd=work)
        m = re.search(r"RESULT (-?\d+)", p.stdout)
        return (int(m.group(1)) if m else None), p.stdout + p.stderr

    db.start()

    # ---------- 1. the table in the main schema file ----------
    db.run(schema)
    cols = sql("SELECT column_name, column_type, is_nullable FROM information_schema.columns WHERE table_schema='ecommerce_db' "
               "AND table_name='request_logs' ORDER BY ordinal_position").replace("\n", "|")
    check("columns: log_id, method, path, status, latency_ms, queue_wait_ms, worker_id, user_id (NULL), created_at", cols ==
          "log_id\tbigint\tNO|method\tvarchar(10)\tNO|path\tvarchar(255)\tNO|status\tsmallint\tNO|latency_ms\tdecimal(10,3)\tNO|"
          "queue_wait_ms\tdecimal(10,3)\tNO|worker_id\tint\tNO|user_id\tint\tYES|created_at\ttimestamp\tNO", cols)
    idx = sql("SELECT column_name FROM information_schema.statistics WHERE table_schema='ecommerce_db' AND table_name='request_logs' "
              "AND index_name='idx_request_logs_created'")
    check("index on created_at", idx == "created_at", idx)

    # ---------- 2. the real multi-row INSERT ----------
    sql("INSERT INTO users (name,email,password) VALUES ('a','a@x.com','secret1')")
    sweep = {}
    sweep_out = {}
    for rows_n in (1, 2, 5, 8, 9, 10, 20, 100, 250):  # how many rows (x 8 parameters) can one statement carry?
        sql("DELETE FROM request_logs")
        got, o = insert(rows_n)
        sweep[rows_n] = got
        sweep_out[rows_n] = o
    sql("DELETE FROM request_logs")
    check("sweep: 1, 2, 5, 8, 9, 10, 20, 100 and 250 rows all insert completely", all(sweep[k] == k for k in sweep), str(sweep))
    for k in sweep:
        if sweep[k] != k:
            print("  output for %d rows: %r" % (k, sweep_out[k][-300:]))
            break
    n, out = insert(250)                              # 250 rows x 8 params = 2000 parameters in ONE statement
    check("db_insert_request_logs: 250 rows in one multi-row INSERT (2000 params)", n == 250, out[-200:])
    check("250 rows stored", sql("SELECT COUNT(*) FROM request_logs") == "250")
    check("user_id 0 became NULL (every 3rd row), others kept their id",
          sql("SELECT SUM(user_id IS NULL), SUM(user_id = 5) FROM request_logs") == "84\t166",
          sql("SELECT SUM(user_id IS NULL), SUM(user_id = 5) FROM request_logs"))
    row = sql("SELECT method, path, status, latency_ms, queue_wait_ms, worker_id FROM request_logs WHERE path = '/p4'")
    check("a row: method, path, status, latency (3 decimals), queue wait, worker id",
          row == "GET\t/p4\t200\t2.123\t0.250\t1", row)
    check("created_at is the request time given by the logger, not the insert time",
          sql("SELECT MIN(UNIX_TIMESTAMP(created_at)), MAX(UNIX_TIMESTAMP(created_at)) FROM request_logs") == "1760000000\t1760000249")
    evil = sql("SELECT path FROM request_logs WHERE path LIKE '/x%'")
    # (the mysql client prints a backslash as \\ in batch mode)
    check("quotes / % / backslash in a path are stored as plain data (no SQL injection)",
          evil == "/x'); DROP TABLE users;-- %s \\\\ \"", repr(evil))
    check("the users table is still there", sql("SELECT COUNT(*) FROM users") == "1")

    # ---------- 3. a bad row: the whole batch is refused (atomic), reported as -1 ----------
    before = sql("SELECT COUNT(*) FROM request_logs")
    n, out = insert(10, bad=True)
    check("one row breaking the CHECK (status 99) -> -1 for the batch", n == -1, out[-200:])
    check("nothing from that batch was stored", sql("SELECT COUNT(*) FROM request_logs") == before)

    # ---------- 4. the standalone script on a database that already has data ----------
    old_schema = schema[:schema.index("-- 10. REQUEST_LOGS")]
    assert "request_logs" not in old_schema
    db.run(old_schema)                                 # your current database: everything except request_logs
    sql("INSERT INTO users (name,email,password) VALUES ('keep','keep@x.com','secret1')")
    sql("INSERT INTO cart (user_id) VALUES (1)")
    check("before: no request_logs table",
          sql("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='ecommerce_db' AND table_name='request_logs'") == "0")
    db.run(alter)
    check("alter_003_request_logs.sql creates the table", sql("SELECT COUNT(*) FROM request_logs") == "0")
    check("existing data untouched (users, cart, products)",
          sql("SELECT (SELECT COUNT(*) FROM users), (SELECT COUNT(*) FROM cart), (SELECT COUNT(*) FROM products)") == "1\t1\t52")
    n, out = insert(20)
    check("the logger's INSERT works on the table made by the script", n == 20, out[-200:])
    db.run(alter)                                      # run it again
    check("running the script again is harmless (IF NOT EXISTS keeps the 20 rows)", sql("SELECT COUNT(*) FROM request_logs") == "20")
    failed, err = sql("INSERT INTO request_logs (method,path,status,latency_ms,queue_wait_ms,worker_id) "
                      "VALUES ('GET','/',99,1,1,1)", expect_error=True)
    check("the script's table has the status CHECK too", failed and "chk_request_logs_status" in err, err[:90])
    failed, err = sql("INSERT INTO request_logs (method,path,status,latency_ms,queue_wait_ms,worker_id) "
                      "VALUES ('GET','/',200,-1,1,1)", expect_error=True)
    check("... and the non-negative times CHECK", failed and "chk_request_logs_times" in err, err[:90])
    failed, err = sql("INSERT INTO request_logs (method,path,status,latency_ms,queue_wait_ms,worker_id) "
                      "VALUES ('GET','/p',200,1,1,1)", expect_error=True)
    check("created_at defaults to now when not given", not failed and sql("SELECT COUNT(*) FROM request_logs WHERE created_at > NOW() - INTERVAL 1 MINUTE") == "1")
finally:
    db.stop()
    shutil.rmtree(work, ignore_errors=True)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
