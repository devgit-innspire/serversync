# test_modals_browser.py - the login / sign up popups, tested in a REAL browser (headless Edge or Chrome).
# It renders the real guest home page (real templates through the real route()), loads the real public/style.css and
# public/app.js, then runs a small script in the page that clicks / presses keys and reports what happened.
# No server, no database, no db.conf. Skips (exit 0) if no browser is installed.
#     python tests\test_modals_browser.py
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"C:\Program Files\MySQL\MySQL Server 8.0"
BROWSERS = [r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
            r"C:\Program Files\Google\Chrome\Application\chrome.exe", r"C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe"]
browser = next((b for b in BROWSERS if os.path.exists(b)), None)
if browser is None:
    print("SKIP: no Edge / Chrome found")
    sys.exit(0)
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

work = tempfile.mkdtemp(prefix="serversync_modal_")
fails = 0

def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" | " + extra) if extra else ""))
    if not cond:
        fails += 1

def url(path):
    return "file:///" + path.replace("\\", "/")

try:
    # 1. the real guest home page, rendered by the real code
    exe = os.path.join(work, "pages_render_check.exe")
    r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "pages_render_check.cpp"),
                        "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(BASE, "include"), "-lws2_32", "-lbcrypt",
                        "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr[:2000]
    out = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", cwd=ROOT).stdout
    m = re.search(r"=== home\nSTATUS \d+ SENT \d+\nTYPE [^\n]*\nEXTRA [^\n]*\nPROBLEMS \d+ [^\n]*\nBODY\n(.*?)\n=== END", out, re.S)
    html = m.group(1)

    pub = os.path.join(ROOT, "public")
    html = html.replace('href="/static/style.css"', 'href="%s"' % url(os.path.join(pub, "style.css")))
    html = html.replace('href="/static/favicon.svg"', 'href="%s"' % url(os.path.join(pub, "favicon.svg")))
    html = html.replace('src="/static/app.js"', 'src="%s"' % url(os.path.join(pub, "app.js")))

    # 2. a script that runs after app.js (which is deferred) and writes its findings into the page
    test_js = r"""
<script>
window.addEventListener('load', function () {
  var out = [];
  function ok(name, cond) { out.push((cond ? 'PASS ' : 'FAIL ') + name); }
  function el(id) { return document.getElementById(id); }
  function shown(id) { return getComputedStyle(el(id)).display; }
  function click(node) {                       // returns false when the page called preventDefault()
    return node.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true}));
  }
  var navLogin = document.querySelector('.topright a[data-modal="loginModal"]');
  var navSignup = document.querySelector('.topright a[data-modal="signupModal"]');

  ok('both popups are hidden when the page loads', shown('loginModal') === 'none' && shown('signupModal') === 'none');
  ok('navbar Sign in / Sign up are real links (work without JavaScript)',
     navLogin.getAttribute('href') === '/login' && navSignup.getAttribute('href') === '/signup');

  var followed = click(navLogin);
  ok('click on navbar Login does NOT follow the link (no page load)', followed === false);
  ok('... the login popup is open and visible', el('loginModal').classList.contains('open') && shown('loginModal') === 'flex');
  ok('... the sign up popup stays closed', !el('signupModal').classList.contains('open'));
  ok('... the email field has the focus', document.activeElement && document.activeElement.name === 'email');
  var box = el('loginModal').querySelector('.modal-box').getBoundingClientRect();
  var ov = el('loginModal').getBoundingClientRect();
  var vw = document.documentElement.clientWidth, vh = document.documentElement.clientHeight;   // the visible area
  var info = ' [window ' + vw + 'x' + vh + ', overlay ' + Math.round(ov.left) + ',' + Math.round(ov.top) + ' ' +
             Math.round(ov.width) + 'x' + Math.round(ov.height) + ', box x ' + Math.round(box.left) + '..' +
             Math.round(box.right) + ' y ' + Math.round(box.top) + '..' + Math.round(box.bottom) + ']';
  ok('... the white box is centred horizontally' + info, Math.abs((box.left + box.right) / 2 - vw / 2) < 2);
  ok('... the white box is centred vertically' + info, Math.abs((box.top + box.bottom) / 2 - vh / 2) < 2);
  ok('... the dark overlay covers the whole visible window' + info,
     ov.left <= 0 && ov.top <= 0 && ov.width >= vw - 1 && ov.height >= vh - 1);
  ok('... the box fits inside the window' + info, box.left >= 0 && box.right <= vw && box.top >= 0 && box.bottom <= vh);

  click(el('loginModal').querySelector('[data-modal="signupModal"]'));
  ok('"Sign up" link inside login popup switches to the sign up popup',
     !el('loginModal').classList.contains('open') && el('signupModal').classList.contains('open'));
  ok('... only one popup is open at a time', document.querySelectorAll('.modal.open').length === 1);
  ok('... the name field has the focus', document.activeElement && document.activeElement.name === 'name');

  click(el('signupModal').querySelector('[data-modal="loginModal"]'));
  ok('"Log in" link inside sign up popup switches back', el('loginModal').classList.contains('open') && !el('signupModal').classList.contains('open'));

  click(el('loginModal').querySelector('.modal-box'));
  ok('a click inside the white box keeps the popup open', el('loginModal').classList.contains('open'));
  click(el('loginModal').querySelector('input[name=email]'));
  ok('a click on an input keeps the popup open', el('loginModal').classList.contains('open'));

  click(el('loginModal'));                     // the dark background itself
  ok('a click on the dark background closes the popup', !el('loginModal').classList.contains('open') && shown('loginModal') === 'none');

  click(navSignup);
  ok('navbar Sign Up opens the sign up popup', el('signupModal').classList.contains('open') && shown('signupModal') === 'flex');
  document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape', bubbles: true}));
  ok('Escape closes it', document.querySelectorAll('.modal.open').length === 0);

  click(navLogin);
  click(el('loginModal').querySelector('[data-close]'));
  ok('the x button closes it', document.querySelectorAll('.modal.open').length === 0);

  document.dispatchEvent(new KeyboardEvent('keydown', {key: 'a', bubbles: true}));
  click(navLogin);
  document.dispatchEvent(new KeyboardEvent('keydown', {key: 'a', bubbles: true}));
  ok('other keys do not close it', el('loginModal').classList.contains('open'));
  click(el('loginModal').querySelector('[data-close]'));

  var lf = el('loginModal').querySelector('form'), sf = el('signupModal').querySelector('form');
  ok('login popup form: POST /login with email + password',
     lf.getAttribute('method') === 'post' && lf.getAttribute('action') === '/login' &&
     lf.elements['email'].type === 'email' && lf.elements['password'].type === 'password' && lf.elements.length === 3);
  ok('sign up popup form: POST /signup with name + email + password',
     sf.getAttribute('method') === 'post' && sf.getAttribute('action') === '/signup' &&
     !!sf.elements['name'] && !!sf.elements['email'] && !!sf.elements['password'] && sf.elements.length === 4);
  ok('popup fields are required (browser checks empty fields before sending)', lf.elements['email'].required && lf.elements['password'].required &&
     sf.elements['name'].required && sf.elements['email'].required && sf.elements['password'].required);

  var pre = document.createElement('pre');
  pre.id = 'results';
  pre.textContent = out.join('\n');
  document.body.appendChild(pre);
});
</script>"""
    html = html.replace("</body>", test_js + "\n</body>")
    page = os.path.join(work, "modal_test.html")
    with open(page, "w", encoding="utf-8") as f:
        f.write(html)

    # 3. run it in the browser and read the results out of the DOM
    # own temporary profile: without it a browser that is already open takes the request and prints nothing
    b = subprocess.run([browser, "--headless=new", "--disable-gpu", "--no-first-run", "--user-data-dir=" + os.path.join(work, "profile"),
                        "--window-size=1100,800",
                        "--virtual-time-budget=5000", "--dump-dom", url(page)], capture_output=True, text=True, encoding="utf-8", timeout=120, stdin=subprocess.DEVNULL)
    res = re.search(r'<pre id="results">(.*?)</pre>', b.stdout, re.S)
    if not res:
        print("FAIL the test page produced no results (script error?)", b.returncode, len(b.stdout), b.stdout[-400:], b.stderr[-300:])
        sys.exit(1)
    lines = [l for l in res.group(1).replace("&amp;", "&").split("\n") if l.strip()]
    for l in lines:
        print(l)
        if l.startswith("FAIL"):
            fails += 1
    check("the browser ran all %d popup checks" % len(lines), len(lines) >= 22, str(len(lines)))
finally:
    shutil.rmtree(work, ignore_errors=True)

print("FAILURES:", fails)
sys.exit(1 if fails else 0)
