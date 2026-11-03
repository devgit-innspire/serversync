# test_mobile_layout.py - renders the REAL pages (tests/pages_render_check.cpp: real templates, real route(), fake DB answers),
# loads the real public/style.css, public/admin.css and public/app.js in headless Edge/Chrome at 360, 390, 414 and 768 px wide
# and reports horizontal page scroll and elements that stick out of the screen. It also takes screenshots.
# No server, no database, no db.conf. Skips (exit 0) if no browser is installed.
#     python tests\test_mobile_layout.py [screenshot_folder]
import json, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BROWSERS = [r"C:\Program Files\Google\Chrome\Application\chrome.exe", r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
            r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"]


def browser_works(b):
    # an installed browser that prints nothing in headless mode (it happens) is skipped: try the next one
    try:
        r = subprocess.run([b, "--headless=new", "--disable-gpu", "--dump-dom", "data:text/html,<p id=x>ok</p>"], capture_output=True, text=True, timeout=60)
        return 'id="x"' in r.stdout
    except Exception:
        return False


browser = next((b for b in BROWSERS if os.path.exists(b) and browser_works(b)), None)
if browser is None:
    print("SKIP: no Edge / Chrome found")
    sys.exit(0)
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
shots = sys.argv[1] if len(sys.argv) > 1 else None
WIDTHS = [360, 375, 414, 768]
PAGES = ["home", "home_user", "products_user", "product_user_plenty", "cart", "mb_checkout", "mb_checkout_no_address", "mb_addresses",
         "mb_address_new", "mb_address_edit", "orders", "mb_order_shipped", "mb_order_cancelled", "sessions", "login_get", "mb_login_guest",
         "signup_get", "metrics", "admin", "admin_users", "admin_logs", "ap_list", "ap_new_get", "ap_edit_get", "ac_list", "ao_list",
         "ao_detail_PLACED", "mb_admin_order", "sx_list",
         "pw_signup_get", "pw_signup_weak", "pw_change_get", "pw_change_server_error", "pw_change_weak", "pw_admin_users"]
SHOT_PAGES = {"pw_signup_weak": 390, "pw_change_server_error": 390, "home_user#menu": 390, "home#menu": 390, "home_user": 390, "home": 390, "products_user": 390, "cart": 390, "mb_checkout": 390, "mb_addresses": 390, "orders": 390,
              "mb_order_shipped": 390, "mb_order_cancelled": 360, "sessions": 360, "mb_address_new": 390, "admin": 390, "ao_list": 390, "metrics": 390}
work = tempfile.mkdtemp(prefix="serversync_mobile_")
fails = 0
PUB = "file:///" + os.path.join(ROOT, "public").replace("\\", "/") + "/"

exe = os.path.join(work, "prc.exe")
r = subprocess.run(["g++", "-std=c++20", "-o", exe, os.path.join(ROOT, "tests", "pages_render_check.cpp"), "-I" + os.path.join(ROOT, "include"),
                    "-lws2_32", "-lbcrypt", "-static-libgcc", "-static-libstdc++"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:3000])
    sys.exit(1)
env = dict(os.environ, MOBILE_FULL_CATS="1")      # the real 15 categories on every page: long chip rows
out = subprocess.run([exe], capture_output=True, text=True, encoding="utf-8", cwd=ROOT, env=env).stdout
bodies = {}
for m in re.finditer(r"^=== (\S+)\nSTATUS[^\n]*\nTYPE [^\n]*\nEXTRA [^\n]*\nPROBLEMS [^\n]*\nBODY\n(.*?)\n=== END$", out, re.S | re.M):
    bodies[m.group(1)] = m.group(2)

MEASURE = r"""
function measure(doc, win, label) {
  var W = win.innerWidth, bad = [];
  var all = doc.body.getElementsByTagName('*');
  for (var i = 0; i < all.length; i++) {
    var e = all[i], cs = win.getComputedStyle(e);
    if (cs.display === 'none' || cs.visibility === 'hidden' || e.closest('svg')) continue;
    var b = e.getBoundingClientRect();
    if (b.width === 0 && b.height === 0) continue;
    if (b.right <= W + 1 && b.left >= -1) continue;
    var clipped = false;
    for (var p = e.parentElement; p && p !== doc.body; p = p.parentElement) {
      var ps = win.getComputedStyle(p);
      if (/(auto|scroll|hidden)/.test(ps.overflowX) && p.getBoundingClientRect().right <= W + 1) { clipped = true; break; }
      if (ps.position === 'fixed' && p.classList.contains('drawer-panel')) { clipped = true; break; }
    }
    if (!clipped) bad.push(e.tagName.toLowerCase() + '.' + (e.className && e.className.baseVal === undefined ? e.className : '') + ' ' + Math.round(b.left) + '..' + Math.round(b.right));
  }
  var sw = doc.documentElement.scrollWidth, cw = doc.documentElement.clientWidth;
  return {label: label, innerWidth: W, scrollWidth: sw, clientWidth: cw, pageScroll: sw !== cw, bad: bad.slice(0, 6)};
}
"""

def wrapper(page_file, widths, mode):
    frames = "".join('<iframe id="f%d" src="%s" style="width:%dpx;height:900px;border:0;display:block"></iframe>' % (w, page_file, w) for w in widths)
    script = MEASURE + """
var results = [], loaded = 0, widths = %s;
widths.forEach(function (w) {
  var f = document.getElementById('f' + w);
  f.onload = function () {
    setTimeout(function () {
      var d = f.contentDocument, win = f.contentWindow;
      var res = [measure(d, win, 'base')];
      if (d.querySelector('.drawer')) { d.documentElement.classList.add('drawer-open'); res.push(measure(d, win, 'drawer open')); d.documentElement.classList.remove('drawer-open'); }
      var lm = d.getElementById('loginModal'), sm = d.getElementById('signupModal');
      if (lm) { lm.classList.add('open'); res.push(measure(d, win, 'login popup')); lm.classList.remove('open'); }
      if (sm) { sm.classList.add('open'); res.push(measure(d, win, 'signup popup')); sm.classList.remove('open'); }
      results.push({width: w, checks: res});
      if (++loaded === widths.length) document.getElementById('out').textContent = JSON.stringify(results);
    }, 200);
  };
});
""" % json.dumps(widths)
    if mode == "shot":
        script = ""
    return "<!doctype html><meta charset=utf-8><body style='margin:0;background:#888'>%s<pre id=out></pre><script>%s</script></body>" % (frames, script)

def page_html(name):
    h = bodies[name]
    h = re.sub(r'(href|src)="/static/([^"?]+)(\?v=\d+)?"', lambda m: '%s="%s%s"' % (m.group(1), PUB, m.group(2)), h)
    # measure the final state, not the middle of the slide-in animation
    return h.replace("</head>", "<style>*{transition:none!important;animation:none!important}</style></head>", 1)

def edge(args):
    return subprocess.run([browser, "--headless=new", "--disable-gpu", "--allow-file-access-from-files", "--hide-scrollbars"] + args,
                          capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)

for name in PAGES:
    if name not in bodies:
        print("MISSING render for " + name)
        fails += 1
        continue
    pf = os.path.join(work, name + ".html")
    open(pf, "w", encoding="utf-8").write(page_html(name))
    wf = os.path.join(work, name + "_w.html")
    open(wf, "w", encoding="utf-8").write(wrapper("file:///" + pf.replace("\\", "/"), WIDTHS, "measure"))
    res = edge(["--virtual-time-budget=4000", "--dump-dom", "file:///" + wf.replace("\\", "/")])
    m = re.search(r'<pre id="out">(.*?)</pre>', res.stdout, re.S)
    if not m or not m.group(1).strip():
        print("FAIL %-24s no measurement (browser output empty)" % name)
        fails += 1
        continue
    data = json.loads(m.group(1).replace("&quot;", '"').replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&"))
    problems = []
    for d in data:
        for c in d["checks"]:
            if c["pageScroll"] or c["bad"]:
                problems.append("%dpx [%s] scrollWidth=%d clientWidth=%d %s" % (d["width"], c["label"], c["scrollWidth"], c["clientWidth"], "; ".join(c["bad"])))
    if problems:
        fails += 1
        print("FAIL %-24s" % name)
        for p in problems:
            print("       " + p)
    else:
        nums = "  ".join("%d: %d=%d" % (d["width"], d["checks"][0]["scrollWidth"], d["checks"][0]["clientWidth"]) for d in sorted(data, key=lambda d: d["width"]))
        print("PASS %-22s scrollWidth=clientWidth  %s" % (name, nums))

if shots:
    os.makedirs(shots, exist_ok=True)
    for name, w in SHOT_PAGES.items():
        base, _, frag = name.partition("#")           # "home#menu" = the page with the drawer opened by :target (no JavaScript)
        if base not in bodies:
            continue
        pf = os.path.join(work, base + ".html")
        wf = os.path.join(work, name.replace("#", "_") + "_s.html")
        open(wf, "w", encoding="utf-8").write(wrapper("file:///" + pf.replace("\\", "/") + ("#" + frag if frag else ""), [w], "shot"))
        edge(["--window-size=%d,900" % w, "--virtual-time-budget=3000",
              "--screenshot=" + os.path.join(shots, "%s_%d.png" % (name.replace("#", "_"), w)), "file:///" + wf.replace("\\", "/")])
    print("screenshots in " + shots)

print("FAILED PAGES: %d" % fails if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
