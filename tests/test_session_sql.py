# test_session_sql.py - checks the session SQL (end_reason, CHECK, index, ALTER script, cleanup UPDATE)
#
# Starts its OWN throwaway MySQL server (port 3399, temp data folder that this script creates and deletes).
# It never reads or writes db.conf and never connects to your real database.
#     python tests\test_session_sql.py
import os, re, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
MYSQLD, MYSQL, MYSQLADMIN = (os.path.join(BASE, "bin", n) for n in ("mysqld.exe", "mysql.exe", "mysqladmin.exe"))
PORT = "3399"
CONN = ["--host=127.0.0.1", "--port=" + PORT, "--user=root"]
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def run(sql, db=None, expect_error=False):
    args = [MYSQL] + CONN + (["--database=" + db] if db else []) + ["-N", "-B"]
    r = subprocess.run(args, input=sql, capture_output=True, text=True)
    if expect_error:
        return r.returncode != 0, r.stderr.strip()
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()

def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()

# the schema as it was before this change: remove end_reason, its CHECK and the new index
new_schema = read(r"sql\ecommerce_db.sql")
old_schema = re.sub(r"\s*-- Why the session ended.*?end_reason VARCHAR\(10\) NULL,", "", new_schema, flags=re.S)
old_schema = re.sub(r",\s*(--[^\n]*\n\s*)*CONSTRAINT chk_sessions_end_reason[^\n]*\n\)", "\n)", old_schema)
old_schema = re.sub(r"-- For the cleanup job.*?ON user_sessions\(is_active, expires_at\);", "", old_schema, flags=re.S)
assert "end_reason" not in old_schema and "idx_sessions_active_expires" not in old_schema

# the exact cleanup statement used by close_expired_sessions() in pages.cpp
CLEANUP = ("UPDATE user_sessions SET is_active = 0, logout_time = expires_at, end_reason = 'EXPIRED' "
           "WHERE is_active = 1 AND expires_at < NOW()")
assert "expires_at < NOW()" in read(r"src\pages.cpp") and "end_reason = 'EXPIRED'" in read(r"src\pages.cpp")

data = tempfile.mkdtemp(prefix="serversync_mysql_")
server = None
try:
    subprocess.run([MYSQLD, "--no-defaults", "--initialize-insecure", "--datadir=" + data, "--basedir=" + BASE],
                   check=True, capture_output=True)
    server = subprocess.Popen([MYSQLD, "--no-defaults", "--datadir=" + data, "--basedir=" + BASE, "--port=" + PORT,
                               "--mysqlx=OFF", "--bind-address=127.0.0.1", "--console"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        time.sleep(1)
        if subprocess.run([MYSQL] + CONN + ["-e", "SELECT 1"], capture_output=True).returncode == 0:
            break
    else:
        sys.exit("throwaway MySQL did not start (is port %s free?)" % PORT)

    # --- 1. fresh schema ---
    run(new_schema)
    cols = run("SELECT column_name FROM information_schema.columns WHERE table_schema='ecommerce_db' "
               "AND table_name='user_sessions'")
    check("fresh schema has end_reason column", "end_reason" in cols.split())
    idx = run("SELECT column_name FROM information_schema.statistics WHERE table_schema='ecommerce_db' "
              "AND table_name='user_sessions' AND index_name='idx_sessions_active_expires' ORDER BY seq_in_index")
    check("index (is_active, expires_at)", idx.split() == ["is_active", "expires_at"], idx)

    run("INSERT INTO users (name,email,password) VALUES ('t','t@x.com','secret1')", "ecommerce_db")
    ins = "INSERT INTO user_sessions (session_id,user_id,expires_at,end_reason) VALUES ('%s',1,NOW()+INTERVAL 1 HOUR,%s)"
    for i, v in enumerate(["NULL", "'LOGOUT'", "'EXPIRED'"]):
        failed, _ = run(ins % ("ok" + str(i), v), "ecommerce_db", expect_error=True)
        check("CHECK allows end_reason = " + v, not failed)
    failed, err = run(ins % ("bad", "'FOO'"), "ecommerce_db", expect_error=True)
    check("CHECK rejects end_reason = 'FOO'", failed and "3819" in err, err[:80])

    # --- 2. ALTER script on an existing DB with data ---
    run(old_schema)       # recreates ecommerce_db in the OLD shape
    run("INSERT INTO users (name,email,password) VALUES ('t','t@x.com','secret1')", "ecommerce_db")
    run("INSERT INTO user_sessions (session_id,user_id,expires_at,is_active) VALUES "
        "('active1',1,NOW()+INTERVAL 1 HOUR,1)", "ecommerce_db")
    run("INSERT INTO user_sessions (session_id,user_id,expires_at,is_active,logout_time) VALUES "
        "('loggedout1',1,NOW()+INTERVAL 1 HOUR,0,NOW())", "ecommerce_db")
    run(read(r"sql\alter_001_session_end_reason.sql"))
    rows = run("SELECT session_id, is_active, IFNULL(end_reason,'NULL') FROM user_sessions ORDER BY session_id",
               "ecommerce_db").split("\n")
    check("ALTER keeps data, backfills LOGOUT only on ended sessions",
          rows == ["active1\t1\tNULL", "loggedout1\t0\tLOGOUT"], str(rows))
    idx = run("SELECT column_name FROM information_schema.statistics WHERE table_schema='ecommerce_db' "
              "AND table_name='user_sessions' AND index_name='idx_sessions_active_expires' ORDER BY seq_in_index")
    check("ALTER adds the index", idx.split() == ["is_active", "expires_at"])
    failed, err = run("UPDATE user_sessions SET end_reason='FOO' WHERE session_id='active1'", "ecommerce_db",
                      expect_error=True)
    check("ALTER adds the CHECK", failed and "3819" in err)

    # --- 3. cleanup UPDATE ---
    run("INSERT INTO user_sessions (session_id,user_id,expires_at) VALUES "
        "('expired1',1,NOW()-INTERVAL 10 MINUTE),('expired2',1,NOW()-INTERVAL 5 MINUTE)", "ecommerce_db")
    out = run(CLEANUP + "; SELECT ROW_COUNT();", "ecommerce_db")
    check("cleanup closes exactly the 2 expired active sessions", out.strip() == "2", out)
    got = {}
    for line in run("SELECT session_id, is_active, IFNULL(end_reason,'NULL'), IFNULL(logout_time = expires_at,'NULL') "
                    "FROM user_sessions", "ecommerce_db").split("\n"):
        sid, *rest = line.split("\t")
        got[sid] = rest
    check("expired rows: is_active=0, EXPIRED, logout_time = expires_at",
          got["expired1"] == ["0", "EXPIRED", "1"] and got["expired2"] == ["0", "EXPIRED", "1"], str(got))
    check("other sessions untouched",
          got["active1"] == ["1", "NULL", "NULL"] and got["loggedout1"] == ["0", "LOGOUT", "0"], str(got))
    out = run(CLEANUP + "; SELECT ROW_COUNT();", "ecommerce_db")
    check("running cleanup again changes nothing", out.strip() == "0", out)

    # --- 4. idle timeout: the refresh UPDATE used by load_user in pages.cpp ---
    pages = read(r"src\pages.cpp")
    assert "last_activity = NOW(), expires_at = \" + EXPIRES_SQL" in pages
    assert "WHERE session_id = ? AND is_active = 1 AND expires_at > NOW()" in pages
    REFRESH = ("UPDATE user_sessions SET last_activity = NOW(), expires_at = NOW() + INTERVAL 5 MINUTE "
               "WHERE session_id = '%s' AND is_active = 1 AND expires_at > NOW()")
    run("INSERT INTO user_sessions (session_id,user_id,expires_at,is_active) VALUES "
        "('idle1',1,NOW()+INTERVAL 1 MINUTE,1),('dead1',1,NOW()-INTERVAL 1 MINUTE,1),"
        "('off1',1,NOW()+INTERVAL 1 HOUR,0)", "ecommerce_db")
    for sid in ("idle1", "dead1", "off1"):
        run(REFRESH % sid, "ecommerce_db")
    left = {}
    for line in run("SELECT session_id, TIMESTAMPDIFF(SECOND, NOW(), expires_at) FROM user_sessions "
                    "WHERE session_id IN ('idle1','dead1','off1')", "ecommerce_db").split("\n"):
        sid, secs = line.split("\t")
        left[sid] = int(secs)
    check("active session: expires_at pushed to now + 5 min", 290 <= left["idle1"] <= 300, str(left["idle1"]))
    check("expired-but-open session is NOT revived", left["dead1"] < 0, str(left["dead1"]))
    check("logged-out session is NOT revived", 3500 <= left["off1"] <= 3600, str(left["off1"]))
    last = run("SELECT TIMESTAMPDIFF(SECOND, last_activity, NOW()) FROM user_sessions WHERE session_id='idle1'",
               "ecommerce_db")
    check("last_activity set in the same UPDATE", 0 <= int(last) <= 3, last)

    # --- 5. CLIENT_FOUND_ROWS: affected rows = matched rows (what db_exec relies on) ---
    tmp = os.path.join(data, "found_rows")
    os.makedirs(tmp)
    exe = os.path.join(tmp, "found_rows_check.exe")
    mi, ml = os.path.join(BASE, "include"), os.path.join(BASE, "lib")
    comp = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "found_rows_check.cpp"),
                           "-I" + mi, "-L" + ml, "-lmysql", "-lws2_32"], capture_output=True, text=True)
    check("found_rows_check.cpp compiles", comp.returncode == 0, comp.stderr[:200])
    if comp.returncode == 0:
        shutil.copy(os.path.join(ROOT, "libmysql.dll"), tmp)
        def affected(flag, sid):
            r = subprocess.run([exe, flag, sid], capture_output=True, text=True, cwd=tmp)
            return r.stdout.strip() or r.stderr.strip()
        check("without CLIENT_FOUND_ROWS a no-op UPDATE reports 0 (the old behaviour)", affected("0", "active1") == "0",
              affected("0", "active1"))
        check("with CLIENT_FOUND_ROWS it reports 1 (row matched)", affected("1", "active1") == "1",
              affected("1", "active1"))
        check("with CLIENT_FOUND_ROWS an unknown session reports 0", affected("1", "no-such-session") == "0",
              affected("1", "no-such-session"))
    check("db.cpp connects with CLIENT_FOUND_ROWS", "CLIENT_FOUND_ROWS) == nullptr" in read(r"src\db.cpp"))
finally:
    if server is not None:
        subprocess.run([MYSQLADMIN] + CONN + ["shutdown"], capture_output=True)
        try:
            server.wait(timeout=30)
        except Exception:
            server.kill()
    shutil.rmtree(data, ignore_errors=True)   # only the temp folder this script created

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
