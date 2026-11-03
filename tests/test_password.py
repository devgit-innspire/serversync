# test_password.py - the password policy, in three parts:
#   A. the real pages (tests/pages_render_check.cpp: real templates, real route(), fake DB answers): rules, signup, login, /account/password,
#      the rules list (hidden until needed), and that the removed "forced change" feature is really gone
#   B. the real password_change() (src/db.cpp) on a throwaway MySQL (see throwaway_mysql.py): wrong current password changes nothing,
#      the right one updates the password (still plain text), and ends ALL OTHER sessions in one transaction
#   C. the live checklist and the eye button (public/app.js) in a real browser (headless Edge / Chrome), cross-checked against the server rules
# No server, no db.conf, never your real database.     python tests\test_password.py
import json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import throwaway_mysql as t

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MYSQL_DIR = r"C:\Program Files\MySQL\MySQL Server 8.0"
work = tempfile.mkdtemp(prefix="serversync_password_")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  | " + str(detail)[:300]))
    if not ok:
        fails += 1


# ------------------------------------------------------------------ A. pages
exe = os.path.join(work, "prc.exe")
r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "pages_render_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-lws2_32", "-lbcrypt", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
out = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", cwd=ROOT).stdout
pages = {}
for m in re.finditer(r"^=== (\S+)\nSTATUS (\d+) SENT (\d+)\nTYPE [^\n]*\nEXTRA ([^\n]*)\nPROBLEMS (\d+) ([^\n]*)\nBODY\n(.*?)\n=== END$", out, re.S | re.M):
    pages[m.group(1)] = dict(status=int(m.group(2)), extra=m.group(4), problems=int(m.group(5)), body=m.group(7))
pwcheck = {}
for line in out.splitlines():
    if line.startswith("PWCHECK "):
        _, label, n, *rest = line.split(" ", 3)
        pwcheck[label] = (int(n), rest[0] if rest else "")


def pg(n):
    return pages[n]


def flash(n):
    m = re.search(r'<div class="flash err">(.*?)</div>', pg(n)["body"], re.S)
    return m.group(1) if m else ""


print("--- A. check_password and the pages")
check("no template problems on any password page", all(p["problems"] == 0 for n, p in pages.items() if n.startswith("pw_")))
check("good password passes", pwcheck["good"][0] == 0)
check("length 8..64 (7 and 65 fail, 64 passes)", pwcheck["short"][0] == 1 and pwcheck["len65"][0] == 1 and pwcheck["len64"][0] == 0)
check("no spaces (space and tab)", "spaces" in pwcheck["space"][1] and "spaces" in pwcheck["tab"][1])
check("needs upper, lower, digit, special (one message each)", all(pwcheck[k][0] == 1 for k in ("no_upper", "no_lower", "no_digit", "no_special")))
check("underscore counts as special", pwcheck["underscore_is_special"][0] == 0)
check("name word, full name and email part are refused; a 2-letter name is ignored",
      all("name or the first part of your email" in pwcheck[k][1] for k in ("has_name_word", "has_full_name", "has_email_local")) and pwcheck["short_name_ignored"][0] == 0)
check("common passwords refused, case-insensitive (Password@123, ADMIN@123, qwerty@123)",
      all("too common" in pwcheck[k][1] for k in ("common_Password@123", "common_ADMIN@123", "common_qwerty")))
check("changing: same as the old password refused; different (even only in case) is fine",
      "different from your current" in pwcheck["same_as_old"][1] and pwcheck["different_from_old"][0] == 0 and pwcheck["old_case_differs_is_different"][0] == 0)
check("non-ASCII characters are allowed and counted as characters", pwcheck["non_ascii_counts_as_chars"][0] == 0)
check("empty password fails every basic rule", pwcheck["empty"][0] == 5)

check("signup page: password + confirm fields, checklist, hidden eye", all(x in pg("pw_signup_get")["body"] for x in
      ('name="password"', 'name="confirm_password"', 'class="pw-rules"', 'data-rule="len"', 'data-rule="match"', 'data-common="password@123|admin@123',
       'class="pw-eye"', 'hidden>')))
check("the sign-up popup has the same fields", all(x in pg("home")["body"][pg("home")["body"].index('id="signupModal"'):] for x in
      ('name="confirm_password"', 'class="pw-rules"', 'name="name"')))
check("weak signup: 400 and EVERY failed rule is listed", pg("pw_signup_weak")["status"] == 400 and all(x in flash("pw_signup_weak") for x in
      ("<ul", "8 to 64 characters", "uppercase", "number", "special character")) and "lowercase" not in flash("pw_signup_weak"))
check("weak signup keeps name and email (escaped) and NEVER refills a password",
      'value="Bob &lt;i&gt;"' in pg("pw_signup_weak")["body"] and 'value="bob@x.com"' in pg("pw_signup_weak")["body"]
      and not re.search(r'type="password"[^>]*value=', pg("pw_signup_weak")["body"]) and 'value="weak"' not in pg("pw_signup_weak")["body"])
check("mismatch is reported", pg("pw_signup_mismatch")["status"] == 400 and "do not match" in flash("pw_signup_mismatch"))
check("name inside the password is reported", "name or the first part" in flash("pw_signup_contains_name"))
check("bad email and weak password are reported together", "valid email" in flash("pw_signup_bad_email_weak") and "8 to 64" in flash("pw_signup_bad_email_weak"))
check("missing confirm -> plain 'Please fill in all fields.'", flash("pw_signup_no_confirm") == "Please fill in all fields.")
check("strong signup -> 302 /login?registered=1", pg("pw_signup_ok")["status"] == 302 and "Location: /login?registered=1" in pg("pw_signup_ok")["extra"])

check("login with a weak (but correct) password still works: the rules are NOT checked at login -> /products, cookie set",
      pg("pw_login_weak")["status"] == 302 and "Location: /products" in pg("pw_login_weak")["extra"] and "Set-Cookie: session_id=" in pg("pw_login_weak")["extra"])
check("login with a strong password -> /products", "Location: /products" in pg("pw_login_strong")["extra"])
check("wrong password -> the same old message, status 401, policy never mentioned",
      pg("pw_login_wrong")["status"] == 401 and flash("pw_login_wrong") == "Invalid email or password.")

check("change page: three fields + checklist with the 'different from current' rule", all(x in pg("pw_change_get")["body"] for x in
      ('name="current_password"', 'name="new_password"', 'name="confirm_password"', 'data-rule="different"', 'data-name=', 'data-email="ann@x.com"')))
check("change page: flash 'Password changed' after success", '<div class="flash ok">Password changed</div>' in pg("pw_change_get_ok")["body"])
check("guest -> /login (GET and POST), PUT -> 405", "Location: /login" in pg("pw_change_guest")["extra"] and "Location: /login" in pg("pw_change_guest_post")["extra"]
      and pg("pw_change_put")["status"] == 405)
check("weak / mismatch / same-as-old / no current -> 400 with the reasons, and none of them reaches the database",
      all(pg(n)["status"] == 400 for n in ("pw_change_weak", "pw_change_mismatch", "pw_change_same", "pw_change_no_current"))
      and "PW_BAD_CALLS 0" in out and "do not match" in flash("pw_change_mismatch") and "different from your current" in flash("pw_change_same")
      and "current password" in flash("pw_change_no_current"))
check("wrong current password -> 403 'Current password is incorrect.'", pg("pw_change_wrong_current")["status"] == 403 and "Current password is incorrect." in flash("pw_change_wrong_current"))
check("busy -> 503, DB error -> 500", pg("pw_change_busy")["status"] == 503 and pg("pw_change_dberror")["status"] == 500)
check("success -> 302 /account/password?ok=1 and password_change got (user 5, current, new, THIS session)",
      pg("pw_change_ok")["status"] == 302 and "Location: /account/password?ok=1" in pg("pw_change_ok")["extra"]
      and ("PASSWORD_CALL 5|Old@pass1|Zx9#kLm2Qw|" + "a" * 64) in out)
check("the password never appears in any page body", not any("Zx9#kLm2Qw" in p["body"] or "Old@pass1" in p["body"] for p in pages.values()))

check("a normal logged-in user is not redirected anywhere", pg("pw_not_forced_products")["status"] == 200)
check("admin Users page has no password button and the old route is gone (404)",
      "Require password change" not in pg("pw_admin_users")["body"] and "require-pw" not in pg("pw_admin_users")["body"] and pg("pw_admin_require_gone")["status"] == 404)
SRC = "".join(open(os.path.join(ROOT, f), encoding="utf-8").read() for f in ("src/pages.cpp", "src/db.cpp", "src/threads.cpp", "src/server.cpp", "include/common.h",
                                                                          "public/app.js", "public/style.css", "sql/ecommerce_db.sql"))
SRC += "".join(open(os.path.join(ROOT, "templates", f), encoding="utf-8").read() for f in os.listdir(os.path.join(ROOT, "templates")))
check("forced password change is removed everywhere: no must_change_pw, no require-pw, no admin_pw templates",
      "must_change_pw" not in SRC and "require-pw" not in SRC and "admin_pw_" not in SRC and "USER_REQUIRE_PW" not in SRC)
check("the migration that drops the column exists", "DROP COLUMN must_change_pw" in open(os.path.join(ROOT, "sql", "migrations", "alter_011_drop_must_change_pw.sql"), encoding="utf-8").read())

# rules list: hidden by default, one hint line, only the failed rules after a server error
def rule_items(name):
    body = pg(name)["body"]
    ul = re.search(r'<ul class="pw-rules([^"]*)"[^>]*>(.*?)</ul>', body, re.S)
    items = re.findall(r'<li data-rule="(\w+)" class="([^"]*)">', ul.group(2))
    return ul.group(1), items


state, items = rule_items("pw_signup_get")
check("signup page: rules list is not marked open or server, all rules are plain 'bad' (css keeps it hidden)", state == "" and all(c == "bad" for _, c in items) and len(items) == 9, (state, items))
check("signup page: one short hint line", "8+ characters with A-Z, a-z, 0-9 and a symbol" in pg("pw_signup_get")["body"])
check("popup (home) and change page have the hint and a hidden list too",
      "8+ characters with A-Z, a-z, 0-9 and a symbol" in pg("home")["body"] and rule_items("pw_change_get")[0] == "" and len(rule_items("pw_change_get")[1]) == 10)
CSS = open(os.path.join(ROOT, "public", "style.css"), encoding="utf-8").read()
check("css: list hidden by default, shown with .open / .server, hint hidden when JavaScript runs, passed rules hidden in server mode",
      ".pw-rules { display: none; }" in CSS and ".pw-rules.open, .pw-rules.server { display: block; }" in CSS and "html.js .pw-hint { display: none; }" in CSS
      and ".pw-rules li.pw-passed { display: none; }" in CSS)
state, items = rule_items("pw_signup_weak")
shown = sorted(k for k, c in items if c != "pw-passed")
check("signup server error: list is open and ONLY the failed rules are visible (len, upper, digit, special for 'weak')",
      state == " server" and shown == ["digit", "len", "special", "upper"] and all(c == "bad" for k, c in items if k in shown), (state, items))
state, items = rule_items("pw_signup_mismatch")
check("mismatch error: only 'match' is visible", state == " server" and [k for k, c in items if c != "pw-passed"] == ["match"], (state, items))
state, items = rule_items("pw_change_server_error")
check("change page server error: only the failed rules are visible (upper, special for 'alllowercase1')",
      state == " server" and sorted(k for k, c in items if c != "pw-passed") == ["special", "upper"], (state, items))
state, items = rule_items("pw_signup_bad_email_weak")
check("an email-only error (good password) does not open the list", rule_items("pw_signup_no_confirm")[0] == "", rule_items("pw_signup_no_confirm"))
hu = pg("home_user")["body"]
check("account bar and mobile drawer link to /account/password", hu.count('href="/account/password"') == 2)

# ------------------------------------------------------------------ B. password_change on a throwaway MySQL
print("--- B. password_change (throwaway MySQL)")
pexe = os.path.join(work, "password_check.exe")
r = subprocess.run(["g++", "-std=c++20", "-o", pexe, os.path.join(ROOT, "tests", "password_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-I" + MYSQL_DIR + "\\include", "-L" + MYSQL_DIR + "\\lib", "-lmysql", "-lws2_32", "-lbcrypt"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
env = dict(os.environ)
env["PATH"] = MYSQL_DIR + "\\lib;" + env["PATH"]


def run_check(mode):
    p = subprocess.run([pexe, mode], capture_output=True, text=True, env=env)
    res = {}
    for line in p.stdout.splitlines():
        k, _, v = line.partition(" ")
        res[k] = v
    return res


t.start()
try:
    t.run(open(os.path.join(ROOT, "sql", "ecommerce_db.sql"), encoding="utf-8").read())
    q = lambda sql: t.run(sql, db="ecommerce_db")
    sess = lambda c: c * 64
    q("INSERT INTO users(email,name,password) VALUES('a@x.in','Ann','Old@pass1'),('b@x.in','Bob','Other@pass1')")
    q("INSERT INTO user_sessions(session_id,user_id,expires_at) VALUES('%s',1,NOW()+INTERVAL 1 DAY),('%s',1,NOW()+INTERVAL 1 DAY),('%s',1,NOW()+INTERVAL 1 DAY),('%s',2,NOW()+INTERVAL 1 DAY)"
      % (sess("a"), sess("b"), sess("c"), sess("d")))
    q("INSERT INTO user_sessions(session_id,user_id,expires_at,is_active,end_reason,logout_time) VALUES('%s',1,NOW(),0,'EXPIRED',NOW())" % sess("e"))

    res = run_check("fail")
    check("wrong current / wrong case / unknown user / someone else's password -> 403", all(res[k].startswith("403") for k in ("WRONG", "WRONG_CASE", "NO_USER", "OTHER_USERS_PASSWORD")), res)
    check("failed attempts changed nothing (password and all 4 sessions still active)",
          q("SELECT password FROM users WHERE user_id=1") == "Old@pass1" and q("SELECT COUNT(*) FROM user_sessions WHERE is_active=1") == "4")

    res = run_check("ok")
    check("right current password -> 200, two other sessions ended", res["OK"].startswith("200 ended=2"), res)
    check("password stored exactly as typed (plain text, no hashing)", q("SELECT password FROM users WHERE user_id=1") == "New@pass9")
    check("current session kept; the other sessions of that user ended with end_reason LOGOUT and a logout_time",
          q("SELECT is_active FROM user_sessions WHERE session_id='%s'" % sess("a")) == "1"
          and q("SELECT CONCAT(is_active, end_reason, (logout_time IS NOT NULL)) FROM user_sessions WHERE session_id IN ('%s','%s') ORDER BY session_id" % (sess("b"), sess("c"))) == "0LOGOUT1\n0LOGOUT1")
    check("other users' sessions and already-ended sessions untouched",
          q("SELECT is_active FROM user_sessions WHERE session_id='%s'" % sess("d")) == "1" and q("SELECT end_reason FROM user_sessions WHERE session_id='%s'" % sess("e")) == "EXPIRED")
    check("the old password no longer works", res["OLD_NOW_WRONG"].startswith("403"), res)
    check("no password rule lives in the database (no CHECK on users.password)",
          q("SELECT COUNT(*) FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA='ecommerce_db' AND CHECK_CLAUSE LIKE '%password%'") == "0")
finally:
    t.stop()

# ------------------------------------------------------------------ C. live checklist + eye in a browser
print("--- C. live checklist and eye button (browser)")
BROWSERS = [r"C:\Program Files\Google\Chrome\Application\chrome.exe", r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
            r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"]


def browser_works(b):
    # a browser that is installed but prints nothing in headless mode (it happens) must not fail the test: try the next one
    try:
        r = subprocess.run([b, "--headless=new", "--disable-gpu", "--dump-dom", "data:text/html,<p id=x>ok</p>"], capture_output=True, text=True, timeout=60)
        return 'id="x"' in r.stdout
    except Exception:
        return False


browser = next((b for b in BROWSERS if os.path.exists(b) and browser_works(b)), None)
if browser is None:
    print("SKIP part C: no working headless Chrome / Edge found")
else:
    PUB = "file:///" + os.path.join(ROOT, "public").replace("\\", "/") + "/"

    def page_html(name):
        h = re.sub(r'(href|src)="/static/([^"?]+)(\?v=\d+)?"', lambda m: '%s="%s%s"' % (m.group(1), PUB, m.group(2)), pg(name)["body"])
        return h

    # the same cases as tests/pages_render_check.cpp (label, password, name, email): the browser result must match the server count
    CASES = [("good", "Zx9#kLm2Qw", "Ann Lee", "ann.lee@x.com"), ("short", "Ab1@xyz", "Ann Lee", "ann@x.com"),
             ("len64", "Aa1@" + "x" * 60, "Ann Lee", "ann@x.com"), ("len65", "Aa1@" + "x" * 61, "Ann Lee", "ann@x.com"),
             ("space", "Zx9# kLm2Qw", "Ann Lee", "ann@x.com"), ("tab", "Zx9#\tkLm2Qw", "Ann Lee", "ann@x.com"),
             ("no_upper", "zx9#klm2qw", "Ann Lee", "ann@x.com"), ("no_lower", "ZX9#KLM2QW", "Ann Lee", "ann@x.com"),
             ("no_digit", "Zxy#kLmpQw", "Ann Lee", "ann@x.com"), ("no_special", "Zx9kLm2QwE", "Ann Lee", "ann@x.com"),
             ("underscore_is_special", "Zx9_kLm2Qw", "Ann Lee", "ann@x.com"), ("has_name_word", "Xx#ANN2025q", "Ann Lee", "bob@x.com"),
             ("has_full_name", "Xx#annlee9Z", "Ann Lee", "bob@x.com"), ("has_email_local", "Xx#Priya99!", "Ann", "priya@x.com"),
             ("short_name_ignored", "Xx#al12345Q", "Al", "al@x.com"), ("common_Password@123", "Password@123", "Ann Lee", "bob@x.com"),
             ("common_ADMIN@123", "ADMIN@123", "Ann Lee", "bob@x.com"), ("common_qwerty", "qwerty@123", "Ann Lee", "bob@x.com"),
             ("non_ascii_counts_as_chars", "Zx9#\u00e9\u20ac\u20b9kLm", "Ann Lee", "bob@x.com"), ("empty", "", "Ann Lee", "bob@x.com")]
    script = """
function rules(form) { var o = {}; form.querySelectorAll('.pw-rules li').forEach(function (li) { o[li.getAttribute('data-rule')] = li.className; }); return o; }
function setv(el, v) { el.value = v; el.dispatchEvent(new Event('input', {bubbles: true})); }
window.addEventListener('load', function () { setTimeout(function () {
  var res = {};
  var f = document.querySelector('main form[action="/signup"]');
  var n = f.querySelector('[name=name]'), e = f.querySelector('[name=email]'), p = f.querySelector('[data-pw=new]'), c = f.querySelector('[data-pw=confirm]');
  var list = f.querySelector('.pw-rules'), hint = f.querySelector('.pw-hint');
  function vis(el) { return getComputedStyle(el).display !== 'none'; }
  res.vis = {};
  res.vis.start = vis(list);                                   // hidden before the user does anything
  res.vis.hint_with_js = vis(hint);                            // the hint is only for no-JavaScript
  p.focus(); res.vis.focused_empty = vis(list);                // focus alone shows it
  p.blur(); res.vis.empty_blur = null;
  res.initial = rules(f);
  res.cases = {};
  %s.forEach(function (cs) { setv(n, cs[2]); setv(e, cs[3]); setv(p, cs[1]); setv(c, cs[1]); res.cases[cs[0]] = rules(f); });
  setv(p, 'Zx9#kLm2Qw'); setv(c, 'different');
  res.mismatch = rules(f).match;
  // visibility walk: typing shows it, leaving the field keeps it while the rules fail, all rules passing hides it, empty + blur hides it
  setv(p, ''); setv(c, ''); p.blur(); c.blur();
  res.vis.empty_blur = vis(list);
  p.focus(); res.vis.focus = vis(list);
  setv(p, 'abc'); res.vis.typing = vis(list);
  p.blur(); c.blur(); res.vis.partial_blur = vis(list);        // still failing: stays visible
  setv(n, 'Ann Lee'); setv(e, 'ann.lee@x.com'); setv(p, 'Zx9#kLm2Qw'); setv(c, 'Zx9#kLm2Qw');
  res.vis.all_pass = vis(list);                                // every rule passes: hidden
  setv(c, 'Zx9#kLm2Qx'); res.vis.pass_then_break = vis(list);  // one rule fails again: shown again
  setv(p, ''); setv(c, ''); p.blur(); c.blur();
  res.vis.cleared_blur = vis(list);
  var eye = f.querySelector('.pw-eye');
  res.eye_visible = !eye.hidden;
  eye.click(); res.after_click = p.type + '/' + eye.getAttribute('aria-pressed');
  eye.click(); res.after_click2 = p.type + '/' + eye.getAttribute('aria-pressed');
  var m = document.querySelector('#signupModal form'); res.modal_rules = m ? m.querySelectorAll('.pw-rules li').length : -1;
  document.getElementById('out').textContent = JSON.stringify(res);
}, 300); });
""" % json.dumps(CASES)
    html = page_html("pw_signup_get").replace("</body>", "<pre id='out'></pre><script>" + script + "</script></body>")
    # (the guest page also holds the hidden popup form: its checklist is set up as well)
    hf = os.path.join(work, "signup_live.html")
    open(hf, "w", encoding="utf-8").write(html)
    res = subprocess.run([browser, "--headless=new", "--disable-gpu", "--virtual-time-budget=5000", "--dump-dom", "file:///" + hf.replace("\\", "/")],
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    m = re.search(r"<pre id=\"out\">(.*?)</pre>", res.stdout, re.S)
    if not m:
        check("browser produced a result", False, res.stdout[-300:] + res.stderr[-300:])
    else:
        data = json.loads(m.group(1).replace("&quot;", '"').replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&"))
        check("before typing every rule is grey (bad)", set(data["initial"].values()) == {"bad"}, data["initial"])
        RULE_KEYS = ("len", "space", "upper", "lower", "digit", "special", "personal", "common")
        ok_all = True
        bad_cases = []
        for label, pw, name, email in CASES:
            js_bad = sum(1 for k in RULE_KEYS if data["cases"][label][k] == "bad")
            server_bad = pwcheck[label][0]
            if label == "empty":
                server_bad = 8                    # empty: the browser also greys the negative rules (space / personal / common) until you type
            if js_bad != server_bad:
                ok_all = False
                bad_cases.append((label, js_bad, server_bad, data["cases"][label]))
        check("live checklist agrees with the server rules for all %d cases" % len(CASES), ok_all, bad_cases)
        check("a good password turns every rule green, including 'both passwords match'", set(data["cases"]["good"].values()) == {"ok"}, data["cases"]["good"])
        check("confirm that differs -> 'match' stays grey", data["mismatch"] == "bad")
        v = data["vis"]
        check("rules list: hidden at start, shown on focus, shown while typing, still shown after leaving a half-typed password",
              v["start"] is False and v["focused_empty"] is True and v["focus"] is True and v["typing"] is True and v["partial_blur"] is True, v)
        check("rules list: hidden when every rule passes, shown again if one fails, hidden when the field is empty and loses focus",
              v["all_pass"] is False and v["pass_then_break"] is True and v["empty_blur"] is False and v["cleared_blur"] is False, v)
        check("with JavaScript the one-line hint is hidden (the list replaces it)", v["hint_with_js"] is False, v)
        check("the eye button is shown by JavaScript and toggles the field type", data["eye_visible"] and data["after_click"] == "text/true" and data["after_click2"] == "password/false", data)
        check("the sign-up popup has its own checklist (9 rules)", data["modal_rules"] == 9, data["modal_rules"])

if browser is not None:
    def browse(html_text, name):
        f = os.path.join(work, name)
        open(f, "w", encoding="utf-8").write(html_text)
        r = subprocess.run([browser, "--headless=new", "--disable-gpu", "--virtual-time-budget=5000", "--dump-dom", "file:///" + f.replace("\\", "/")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        m = re.search(r"<pre id=\"out\">(.*?)</pre>", r.stdout, re.S)
        return json.loads(m.group(1).replace("&quot;", '"').replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")) if m else None

    probe = """
function vis(el) { return getComputedStyle(el).display !== 'none'; }
window.addEventListener('load', function () { setTimeout(function () {
  var list = document.querySelector('main .pw-rules'), hint = document.querySelector('main .pw-hint');
  var shown = []; list.querySelectorAll('li').forEach(function (li) { if (vis(li)) shown.push(li.getAttribute('data-rule')); });
  var res = {list: vis(list), hint: vis(hint), shown: shown, js: document.documentElement.classList.contains('js')};
  var pw = document.querySelector('main [data-pw=new]');
  pw.focus();
  var after = []; list.querySelectorAll('li').forEach(function (li) { if (vis(li)) after.push(li.getAttribute('data-rule')); });
  res.focus_list = vis(list); res.focus_shown = after.length; res.server_class = list.classList.contains('server');
  document.getElementById('out').textContent = JSON.stringify(res);
}, 300); });
"""
    def with_probe(name, drop_app_js=False):
        h = page_html(name)
        if drop_app_js:
            h = re.sub(r'<script src="[^"]*app\.js[^"]*" defer></script>', '', h)
        return h.replace("</body>", "<pre id='out'></pre><script>" + probe + "</script></body>")

    d = browse(with_probe("pw_signup_weak"), "server_error_js.html")
    check("server error + JavaScript: list open with ONLY the 4 failed rules, hint hidden; focusing a field switches to the full live list",
          d is not None and d["list"] is True and sorted(d["shown"]) == ["digit", "len", "special", "upper"] and d["hint"] is False and d["js"] is True
          and d["server_class"] is False and d["focus_shown"] == 9, d)
    d = browse(with_probe("pw_signup_get", drop_app_js=True), "nojs_get.html")
    check("without JavaScript (no app.js): only the one-line hint is visible, the rules list is hidden", d is not None and d["list"] is False and d["hint"] is True and d["js"] is False, d)
    d = browse(with_probe("pw_signup_weak", drop_app_js=True), "nojs_error.html")
    check("without JavaScript after a server error: the hint and ONLY the failed rules are visible",
          d is not None and d["list"] is True and d["hint"] is True and sorted(d["shown"]) == ["digit", "len", "special", "upper"], d)
    d = browse(with_probe("pw_change_server_error", drop_app_js=True), "nojs_change.html")
    check("change page without JavaScript after an error: only the failed rules", d is not None and sorted(d["shown"]) == ["special", "upper"], d)

print("FAILED: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
