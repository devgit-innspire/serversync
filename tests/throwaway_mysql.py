# throwaway_mysql.py - helper for tests: starts its OWN temporary MySQL server (port 3399, temp data folder).
# Never reads or writes db.conf and never connects to your real database.
import os, shutil, subprocess, tempfile, time

BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
MYSQLD, MYSQL, MYSQLADMIN = (os.path.join(BASE, "bin", n) for n in ("mysqld.exe", "mysql.exe", "mysqladmin.exe"))
PORT = "3399"
CONN = ["--host=127.0.0.1", "--port=" + PORT, "--user=root"]

_data = None
_server = None

def start():
    global _data, _server
    _data = tempfile.mkdtemp(prefix="serversync_mysql_")
    subprocess.run([MYSQLD, "--no-defaults", "--initialize-insecure", "--datadir=" + _data, "--basedir=" + BASE],
                   check=True, capture_output=True)
    _server = subprocess.Popen([MYSQLD, "--no-defaults", "--datadir=" + _data, "--basedir=" + BASE, "--port=" + PORT,
                                "--mysqlx=OFF", "--bind-address=127.0.0.1", "--console",
                                # small memory use: the test PC is often short of RAM
                                "--performance-schema=OFF", "--innodb-buffer-pool-size=32M", "--innodb-log-buffer-size=4M"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        time.sleep(1)
        if subprocess.run([MYSQL] + CONN + ["-e", "SELECT 1"], capture_output=True).returncode == 0:
            return
    stop()
    raise RuntimeError("throwaway MySQL did not start (is port %s free?)" % PORT)

def stop():
    global _data, _server
    if _server is not None:
        subprocess.run([MYSQLADMIN] + CONN + ["shutdown"], capture_output=True)
        try:
            _server.wait(timeout=30)
        except Exception:
            _server.kill()
        _server = None
    if _data is not None:
        shutil.rmtree(_data, ignore_errors=True)    # only the temp folder created above
        _data = None

def popen(sql, db="ecommerce_db"):
    """start a mysql client running `sql` in the background (for concurrency tests)"""
    p = subprocess.Popen([MYSQL] + CONN + ["--database=" + db, "-N", "-B"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p.stdin.write(sql)
    p.stdin.close()
    return p

def run(sql, db=None, expect_error=False):
    args = [MYSQL] + CONN + (["--database=" + db] if db else []) + ["-N", "-B"]
    r = subprocess.run(args, input=sql, capture_output=True, text=True)
    if expect_error:
        return r.returncode != 0, r.stderr.strip()
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()
