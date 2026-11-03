# test_roles.py - tests role_change() in src/db.cpp (admin panel: promote / demote) on a throwaway MySQL.
# Never reads or writes db.conf and never touches your real database (see throwaway_mysql.py).
#     python tests\test_roles.py
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

work = tempfile.mkdtemp(prefix="serversync_roles_")

def build():
    exe = os.path.join(work, "roles_check.exe")
    for attempt in range(6):            # g++ sometimes runs out of memory when the PC is busy: just try again
        r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "roles_check.cpp"),
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
    res = [{"status": int(m[2]), "code": m[3], "from": m[4]} for m in re.finditer(r"RESULT job=(\d+) status=(\d+) code=(\S*) from=(\S*)", r.stdout)]
    return res, re.findall(r"AUTOCOMMIT conn=\d value=(\d)", r.stdout), r.stdout + r.stderr

def fresh(admins=(1, 2), customers=(3, 4)):
    db.run(read(r"sql\ecommerce_db.sql"))
    for i in sorted(admins + customers):
        sql("INSERT INTO users (name,email,password,role) VALUES ('u%d','u%d@x.com','x','%s')" % (i, i, "ADMIN" if i in admins else "CUSTOMER"))

def role(uid): return sql("SELECT role FROM users WHERE user_id = %d" % uid)
def admins(): return int(sql("SELECT COUNT(*) FROM users WHERE role = 'ADMIN'"))
def audit_rows(): return sql("SELECT admin_id, action, details FROM admin_actions ORDER BY action_id").split("\n") if sql("SELECT COUNT(*) FROM admin_actions") != "0" else []

try:
    db.start()
    exe = build()

    fresh()
    res, ac, out = run_jobs(exe, ["1:3:ADMIN"])
    check("an admin promotes a customer: 200, old role reported", res and res[0]["status"] == 200 and res[0]["from"] == "CUSTOMER", str(res))
    check("... the user is ADMIN now", role(3) == "ADMIN")
    check("... one audit row, written in the same transaction: admin 1, USER_ROLE, 'user 3 role CUSTOMER->ADMIN'",
          audit_rows() == ["1\tUSER_ROLE\tuser 3 role CUSTOMER->ADMIN"], str(audit_rows()))
    check("... autocommit ON on every pooled connection", ac == ["1"] * 4, str(ac))
    res, ac, out = run_jobs(exe, ["1:3:CUSTOMER"])
    check("an admin demotes another admin (3 admins -> 2): 200", res and res[0]["status"] == 200 and role(3) == "CUSTOMER" and admins() == 2, str(res))

    res, ac, out = run_jobs(exe, ["1:1:CUSTOMER"])
    check("an admin cannot change their OWN role: 400 own_role, still ADMIN", res and res[0]["status"] == 400 and res[0]["code"] == "own_role" and role(1) == "ADMIN", str(res))
    res, ac, out = run_jobs(exe, ["1:1:ADMIN"])
    check("... also not 'to the same role' for yourself", res and res[0]["code"] == "own_role", str(res))
    n_audit = len(audit_rows())
    res, ac, out = run_jobs(exe, ["1:2:ADMIN"])
    check("promoting someone who is already ADMIN: 400 same_role", res and res[0]["status"] == 400 and res[0]["code"] == "same_role", str(res))
    res, ac, out = run_jobs(exe, ["1:4:CUSTOMER"])
    check("demoting a customer: 400 same_role", res and res[0]["status"] == 400 and res[0]["code"] == "same_role")
    res, ac, out = run_jobs(exe, ["1:999:ADMIN"])
    check("unknown user: 404 no_user", res and res[0]["status"] == 404 and res[0]["code"] == "no_user", str(res))
    for bad in ("SUPERUSER", "admin", ""):
        res, ac, out = run_jobs(exe, ["1:4:" + bad])
        check("invalid role '%s': 400 bad_request" % bad, res and res[0]["status"] == 400 and res[0]["code"] == "bad_request", str(res))
    res, ac, out = run_jobs(exe, ["4:3:ADMIN"])
    check("a CUSTOMER (not admin) cannot change roles: 403 not_admin, nothing changed", res and res[0]["status"] == 403 and res[0]["code"] == "not_admin" and role(3) == "CUSTOMER", str(res))
    check("refused changes write no audit row", len(audit_rows()) == n_audit, str(audit_rows()))

    # the last admin can never be demoted: two admins demote each other at the same moment, many times
    ok_every_time = True
    detail = []
    for i in range(8):
        fresh(admins=(1, 2), customers=(3,))
        res, ac, out = run_jobs(exe, ["1:2:CUSTOMER", "2:1:CUSTOMER"])
        codes = sorted(r["status"] for r in res)
        detail.append((codes, admins()))
        if not (codes == [200, 403] and admins() == 1):
            ok_every_time = False
    check("two admins demote each other at the same time (8 rounds): exactly one wins, ONE admin always remains, never zero", ok_every_time, str(detail))
    fresh(admins=(1, 2, 3), customers=(4,))
    res, ac, out = run_jobs(exe, ["1:2:CUSTOMER", "2:3:CUSTOMER", "3:1:CUSTOMER"])
    check("three admins demote each other in a circle: at least one admin is left", admins() >= 1, str(res) + " admins=" + str(admins()))
    check("... and every success has its audit row (no more rows than successes)", len(audit_rows()) == sum(1 for r in res if r["status"] == 200), str(audit_rows()))
    check("... autocommit ON everywhere", ac == ["1"] * 4, str(ac))
    fresh(admins=(1,), customers=(2,))
    res, ac, out = run_jobs(exe, ["1:2:ADMIN", "2:1:CUSTOMER"])
    check("promote and be demoted at once: never zero admins, 'locked order' keeps the result consistent", admins() >= 1, str(res))

    # a failure in the audit insert rolls the role change back
    fresh()
    sql("RENAME TABLE admin_actions TO admin_actions_hidden")
    res, ac, out = run_jobs(exe, ["1:3:ADMIN"])
    check("audit row cannot be written: 500, and the role change is rolled back (still CUSTOMER)", res and res[0]["status"] == 500 and role(3) == "CUSTOMER", str(res))
    check("... logged with the reason; autocommit ON", "ROLLBACK reason" in out and ac == ["1"] * 4, out[-300:])
    sql("RENAME TABLE admin_actions_hidden TO admin_actions")
    res, ac, out = run_jobs(exe, ["1:3:ADMIN"])
    check("works again once the table is back (connection was clean)", res and res[0]["status"] == 200 and role(3) == "ADMIN", str(res))

    # lock wait timeout: retried 3 times, then busy
    sql("SET GLOBAL innodb_lock_wait_timeout = 1")
    fresh()
    blocker = db.popen("START TRANSACTION; SELECT * FROM users WHERE role = 'ADMIN' FOR UPDATE; SELECT SLEEP(6); COMMIT;")
    time.sleep(0.5)
    res, ac, out = run_jobs(exe, ["1:3:ADMIN"])
    blocker.wait()
    check("admin rows locked for 6 s: 503 busy after 3 attempts, nothing changed", res and res[0]["status"] == 503 and res[0]["code"] == "busy" and role(3) == "CUSTOMER", str(res))
    check("... retries logged, autocommit ON", "retry 2/3" in out and "retry 3/3" in out and "gave up after 3 attempts" in out and ac == ["1"] * 4, out[-300:])

    dbsrc = read(r"src\db.cpp")
    check("source: ADMIN rows read FOR UPDATE before counting, target row FOR UPDATE, UPDATE ... AND role = <old>, actor must still be admin",
          "SELECT user_id FROM users WHERE role = 'ADMIN' ORDER BY user_id FOR UPDATE" in dbsrc and "SELECT role FROM users WHERE user_id = ? FOR UPDATE" in dbsrc and
          "UPDATE users SET role = ? WHERE user_id = ? AND role = ?" in dbsrc and "admins.size() <= 1" in dbsrc and '"not_admin"' in dbsrc)
finally:
    shutil.rmtree(work, ignore_errors=True)
    db.stop()

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
