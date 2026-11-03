# test_db_debug.py - DEBUG_SQL: default 0 (logs nothing) and, when 1, passwords / emails / session ids are masked.
# Needs no database and no db.conf.
#     python tests\test_db_debug.py
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

work = tempfile.mkdtemp(prefix="serversync_dbg_")
try:
    def run(debug):
        inc = os.path.join(work, "inc%d" % debug)
        shutil.copytree(os.path.join(ROOT, "include"), inc)
        p = os.path.join(inc, "common.h")
        t = open(p, encoding="utf-8").read()
        assert "const int DEBUG_SQL = 0;" in t
        open(p, "w", encoding="utf-8").write(t.replace("const int DEBUG_SQL = 0;", "const int DEBUG_SQL = %d;" % debug))
        exe = os.path.join(work, "dbg%d.exe" % debug)
        r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "db_debug_check.cpp"),
                            "-I" + inc, "-I" + os.path.join(BASE, "include"), "-L" + os.path.join(BASE, "lib"),
                            "-lmysql", "-lws2_32", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr[:2000]
        shutil.copy(os.path.join(ROOT, "libmysql.dll"), work)
        return subprocess.run([exe], capture_output=True, text=True, cwd=work).stdout

    check("DEBUG_SQL defaults to 0 in common.h", "const int DEBUG_SQL = 0;" in open(os.path.join(ROOT, "include", "common.h"), encoding="utf-8").read())

    off = run(0)
    check("DEBUG_SQL = 0: nothing is logged", "[DB]" not in off, off[:200])

    on = run(1)
    print(on)
    check("login: password and email are masked", "hunter22" not in on and "secret@x.com" not in on)
    check("login: params shown as '***' with their lengths",
          "param 1 = '***' (len 12, bound length 12)" in on and "param 2 = '***' (len 8, bound length 8)" in on)
    check("signup: name, email and password are all masked",
          "p4ssw0rd" not in on and "bob@x.com" not in on and "Bobby Tables" not in on)
    check("session lookup/insert: session ids are masked", "a" * 64 not in on and "b" * 64 not in on)
    check("session insert: even the user id param is masked (whole statement is secret)", "param 2 = '***' (len 1" in on)
    check("orders insert: values are shown (user_id, cart_id)", "param 1 = '4' (len 1, bound length 1)" in on and
          "param 2 = '7' (len 1, bound length 1)" in on)
    check("orders insert: SQL text shows the 'PLACED' literal", "'PLACED'" in on)
    check("normal query value is shown", "param 1 = '%lap%' (len 5, bound length 5)" in on)
    check("very long values are cut at 200 chars (real length still shown)",
          "x" * 200 in on and "x" * 201 not in on and "(len 500, bound length 500)" in on)
finally:
    shutil.rmtree(work, ignore_errors=True)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
