// pages_helpers_check.cpp - tests the pure helpers in pages.cpp (strip_param, safe_return, return_target)
// without a database: pages.cpp is #included so its static functions are visible, and everything it calls
// outside (DB, sockets, logging) is replaced by stubs below. Never touches db.conf.
//   g++ -std=c++20 -Iinclude tests\pages_helpers_check.cpp -o ... -lws2_32 -lbcrypt   (see test_pages_helpers.py)
#include "../src/pages.cpp"
#include <iostream>

// ---- stubs for the functions pages.cpp calls ----
// send_response remembers what would have been sent, so the tests can look at it
static int sent_status = 0;
static string sent_type, sent_body, sent_extra;
void send_response(SOCKET, int status, const string& type, const string& body, const string& extra) {
    sent_status = status; sent_type = type; sent_body = body; sent_extra = extra;
}
void log_tag(const string&, const string&) {}
int thread_id() { return 0; }
OrderResult order_change(int, const string&, int, bool, const string&) { return OrderResult(); }   // real one: tests/test_orders.py
bool order_transition_ok(const string&, const string&) { return false; }
RoleResult role_change(int, int, const string&) { return RoleResult(); }
void admin_audit(int, const string&, const string&) {}
static long long fake_now = 1000000;      // tests move the clock to check the category cache (60 s)
long long now_us() { return fake_now; }
MYSQL* get_conn(int& id) { id = 0; return nullptr; }
void put_conn(int) {}
bool db_select(const string&, const vector<string>&, vector<vector<string>>&, int*) { return false; }
int db_exec(const string&, const vector<string>&, int*) { return -1; }
bool db_scalar(const string&, string&, int*) { return false; }
static vector<vector<string>> fake_rows;        // what db_select_on "finds"
bool db_select_on(MYSQL*, const string&, const vector<string>&, vector<vector<string>>& rows, int*) {
    rows = fake_rows;
    return true;
}
CheckoutResult do_checkout(int) { return CheckoutResult(); }
int db_exec_on(MYSQL*, const string&, const vector<string>&, int*) { return -1; }
int find_active_cart(MYSQL*, int) { return 0; }
int get_active_cart(MYSQL*, int) { return 1; }
static int cart_add_calls = 0;
bool cart_add(MYSQL*, int, const string&, const string&) { cart_add_calls++; return true; }
static Metrics fake_metrics;
Metrics metrics_snapshot() { return fake_metrics; }
int cart_set_qty(MYSQL*, int, const string&, const string&) { return -1; }
int cart_remove(MYSQL*, int, const string&) { return -1; }
bool cart_items(MYSQL*, int, vector<vector<string>>&) { return false; }
int cart_count(MYSQL*, int) { return 0; }
string trim(const string& s) {                 // same as server.cpp's (the real one lives in a file we do not include)
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static int fails = 0;
static void check(const string& name, const string& got, const string& want) {
    bool ok = (got == want);
    cout << (ok ? "PASS " : "FAIL ") << name << " | got '" << got << "'" << (ok ? "" : " want '" + want + "'") << endl;
    if (!ok) fails++;
}

// ---- helpers for the template tests ----
static bool has(const string& hay, const string& needle) { return hay.find(needle) != string::npos; }
static void yes(const string& name, bool cond, const string& extra = "") {
    check(name, cond ? "yes" : "no" + (extra.empty() ? "" : " (" + extra + ")"), "yes");
}

int main() {
    // ================= PART 2: template engine =================
    yes("templates/ loads and every required template exists", load_templates("templates"));
    yes("a folder that does not exist -> load_templates is false", !load_templates("no_such_folder_xyz"));
    load_templates("templates");

    templates["t_esc"] = "<p>{{x}}</p>";
    templates["t_raw"] = "<p>{{{x}}}</p>";
    templates["t_two"] = "{{a}}-{{{b}}}-{{a}}";
    templates["t_open"] = "keep {{ this and {{{ that";
    templates["t_space"] = "[{{ k }}][{{{ k }}}]";
    templates["t_nl"] = "line";                      // (the loader strips trailing newlines; checked below)
    const string evil = "<script>alert(\"x\")</script> & 'q'";
    render_problems = 0;
    string e = render("t_esc", {{"x", evil}});
    yes("{{key}} HTML-escapes < > & \" '", e == "<p>&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt; &amp; &#39;q&#39;</p>", e);
    yes("{{{key}}} inserts the value raw", render("t_raw", {{"x", "<b>hi</b>"}}) == "<p><b>hi</b></p>");
    yes("the same key can be used twice, escaped and raw mixed", render("t_two", {{"a", "<"}, {"b", "<i>"}}) == "&lt;-<i>-&lt;");
    yes("keys may have spaces inside the braces", render("t_space", {{"k", "<"}}) == "[&lt;][<]");
    yes("a '{{' without a closing '}}' is kept as plain text", render("t_open", {}) == "keep {{ this and {{{ that");
    yes("no problems so far", render_problems == 0);

    string once = render("t_esc", {{"x", "{{y}}"}});
    yes("a value containing {{y}} is NOT expanded again (no template injection)", once == "<p>{{y}}</p>" && render_problems == 0, once);
    once = render("t_raw", {{"x", "{{{y}}}"}});
    yes("... also not for raw values", once == "<p>{{{y}}}</p>" && render_problems == 0, once);

    render_problems = 0;
    yes("a missing key renders as empty text", render("t_two", {{"a", "1"}}) == "1--1");
    yes("... and is counted as a problem with a readable message", render_problems == 1 && has(render_last_problem, "needs the value 'b'"), render_last_problem);
    render_problems = 0;
    yes("an unknown template renders as empty text and is counted", render("no_such_template", {}).empty() && render_problems == 1 &&
        has(render_last_problem, "not found"));
    render_problems = 0;

    // the loader: trailing newlines are stripped, only .html files, name = file name without .html
    {
        string dir = "tests_tmp_templates";
        filesystem::create_directory(dir);
        ofstream(dir + "/layout.html") << "L\n\n";
        ofstream(dir + "/plain.html") << "P";
        ofstream(dir + "/readme.txt") << "ignored";
        yes("a folder without all required templates -> false", !load_templates(dir));
        yes("only .html files are loaded, names without extension",
            templates.count("layout") == 1 && templates.count("plain") == 1 && templates.count("readme") == 0 &&
            templates.size() == 2);
        yes("trailing newlines of a template file are removed", templates["layout"] == "L");
        filesystem::remove_all(dir);
    }
    load_templates("templates");

    // ---- layout, nav, flash, error ----
    Request guest;
    string pg = page(guest, "My <Title>", "<h1>Body</h1>", "<meta name=\"x\">", flash_box("ok", "Saved"));
    yes("layout: complete html document with lang, charset, viewport", has(pg, "<!doctype html>") && has(pg, "<html lang=\"en\">") &&
        has(pg, "<meta charset=\"utf-8\">") && has(pg, "name=\"viewport\""));
    yes("layout: links /static/style.css and the favicon (no inline <style>)",
        has(pg, "<link rel=\"stylesheet\" href=\"/static/style.css\">") && has(pg, "href=\"/static/favicon.svg\"") && !has(pg, "<style"));
    yes("layout: title is escaped", has(pg, "<title>My &lt;Title&gt; - ServerSync</title>") && !has(pg, "<Title>"));
    yes("layout: head_extra goes into <head>", has(pg, "<meta name=\"x\"><title>"));
    yes("layout: flash area comes before the content", pg.find("class=\"flash ok\"") != string::npos &&
        pg.find("class=\"flash ok\"") < pg.find("<h1>Body</h1>"));
    yes("layout: content is inserted raw, inside <main>", has(pg, "<main class=\"container\">") && has(pg, "<h1>Body</h1>"));
    yes("layout: guest navbar with Sign in / Sign up popups, cart badge 0, no Logout", has(pg, "href=\"/login\" data-modal=\"loginModal\"") && has(pg, "href=\"/signup\" data-modal=\"signupModal\"") &&
        !has(pg, "action=\"/logout\"") && has(pg, "title=\"0 items\""));
    yes("layout: no flash box when there is no flash", !has(page(guest, "t", "c"), "class=\"flash"));

    Request user;
    user.user_id = 3; user.user_name = "Ann <b>Bold</b>"; user.cart_count = 1;
    string pu = page(user, "t", "c");
    yes("user navbar: name is escaped ('Hello, Ann &lt;b&gt;...')", has(pu, "Hello, Ann &lt;b&gt;Bold&lt;/b&gt;") && !has(pu, "<b>Bold</b>"));
    yes("user navbar: cart badge with the count, 'item' for 1", has(pu, "<span class=\"badge\" title=\"1 item\">1</span>"));
    user.cart_count = 5;
    yes("user navbar: 'items' for 5", has(page(user, "t", "c"), "<span class=\"badge\" title=\"5 items\">5</span>"));
    user.cart_count = 0;
    yes("user navbar: badge shows 0 too", has(page(user, "t", "c"), "title=\"0 items\">0</span>"));
    yes("user navbar: Orders, My Sessions, POST logout form, no Login link",
        has(pu, "href=\"/orders\"") && has(pu, "href=\"/sessions\"") && has(pu, "method=\"post\" action=\"/logout\"") && !has(pu, "href=\"/login\""));

    string ep = render_error_page("404 <Not> Found");
    yes("render_error_page: full page with the escaped message in an <h1>", has(ep, "<h1>404 &lt;Not&gt; Found</h1>") && has(ep, "<!doctype html>"));
    yes("redirect body comes from the plain template", render("plain", {{"message", "Redirecting..."}}) ==
        "<!doctype html><html><body>Redirecting...</body></html>");
    yes("none of the above produced a template problem", render_problems == 0, render_last_problem);

    // strip_param
    check("strip: no query", strip_param("/products", "added"), "/products");
    check("strip: only param", strip_param("/products?added=5", "added"), "/products");
    check("strip: keeps others in order", strip_param("/products?cat=3&added=5&q=lap", "added"), "/products?cat=3&q=lap");
    check("strip: param first", strip_param("/product?added=5&id=7", "added"), "/product?id=7");
    check("strip: name must match exactly", strip_param("/p?xadded=1&added2=2", "added"), "/p?xadded=1&added2=2");
    check("strip: empty pairs dropped", strip_param("/p?&&a=1&&", "added"), "/p?a=1");
    check("strip: keeps encoded text", strip_param("/products?q=caf%C3%A9+x&added=1", "added"), "/products?q=caf%C3%A9+x");

    // safe_return: allowed
    check("safe: plain path", safe_return("/products"), "/products");
    check("safe: path with query", safe_return("/products?cat=3&q=lap"), "/products?cat=3&q=lap");
    check("safe: product page", safe_return("/product?id=5"), "/product?id=5");
    check("safe: just slash", safe_return("/"), "/");
    // safe_return: blocked -> /products
    check("block: //evil.com", safe_return("//evil.com"), "/products");
    check("block: //", safe_return("//"), "/products");
    check("block: https://evil.com", safe_return("https://evil.com"), "/products");
    check("block: javascript:", safe_return("javascript:alert(1)"), "/products");
    check("block: empty", safe_return(""), "/products");
    check("block: relative path", safe_return("products"), "/products");
    check("block: backslash /\\evil.com", safe_return("/\\evil.com"), "/products");
    check("block: backslash later", safe_return("/a\\b"), "/products");
    check("block: CRLF header injection", safe_return("/products\r\nSet-Cookie: x=1"), "/products");
    check("block: LF only", safe_return("/a\nb"), "/products");
    check("block: space", safe_return("/a b"), "/products");
    check("block: tab", safe_return("/\t/evil.com"), "/products");
    check("block: NUL", safe_return(string("/a\0b", 4)), "/products");
    check("block: non-ASCII byte", safe_return("/caf\xC3\xA9"), "/products");
    check("block: too long", safe_return("/" + string(1001, 'a')), "/products");

    // return_target: current page without old messages
    Request r;
    r.target = "/products?cat=3&q=lap&added=5&cart_err=stock";
    check("return_target strips added + cart_err", return_target(r), "/products?cat=3&q=lap");
    r.target = "/product?id=5&added=5";
    check("return_target product page", return_target(r), "/product?id=5");
    r.target = "/products";
    check("return_target no query", return_target(r), "/products");

    // the redirect built after add (same expression as cart_add_page)
    auto build = [](const string& ret, const string& pid, const string& problem) {
        string back = strip_param(strip_param(safe_return(ret), "added"), "cart_err");
        back += (back.find('?') == string::npos ? "?" : "&") + string("added=") + pid;
        if (!problem.empty()) back += "&cart_err=" + problem;
        return back;
    };
    check("redirect: with query", build("/products?cat=3&q=lap", "5", ""), "/products?cat=3&q=lap&added=5");
    check("redirect: no query", build("/products", "5", ""), "/products?added=5");
    check("redirect: replaces old added", build("/products?added=9", "5", ""), "/products?added=5");
    check("redirect: stock problem", build("/product?id=5", "5", "stock"), "/product?id=5&added=5&cart_err=stock");
    check("redirect: evil return -> /products", build("//evil.com", "5", ""), "/products?added=5");
    check("redirect: missing return -> /products", build("", "5", ""), "/products?added=5");

    // ---- /metrics and /metrics.json ----
    fake_metrics.uptime_s = 3725; fake_metrics.total = 120;
    fake_metrics.c2xx = 90; fake_metrics.c3xx = 20; fake_metrics.c4xx = 7; fake_metrics.c5xx = 3;
    fake_metrics.rejected_503 = 2; fake_metrics.timeouts_408 = 1;
    fake_metrics.busy_workers = 2; fake_metrics.queue_length = 5; fake_metrics.free_db_conns = 3;
    fake_metrics.latency_samples = 100; fake_metrics.avg_ms = 12.345; fake_metrics.p95_ms = 80.5;
    fake_metrics.rps = 4.25; fake_metrics.checkout_commit = 6; fake_metrics.checkout_rollback = 1;
    fake_metrics.dropped_logs = 9;

    auto get = [](const string& path, const string& ip, const string& method = "GET") {
        Request r;
        r.method = method; r.path = path; r.target = path; r.client_ip = ip;
        sent_status = 0; sent_body = ""; sent_type = ""; sent_extra = "";
        int status = route(0, r);
        return status;
    };

    check("metrics from 127.0.0.1 -> 200", to_string(get("/metrics", "127.0.0.1")), "200");
    check("metrics page auto-refreshes with a meta tag (no JavaScript)",
          sent_body.find("<meta http-equiv=\"refresh\" content=\"2\">") != string::npos &&
          sent_body.find("<script") == string::npos ? "yes" : "no", "yes");
    check("metrics page shows uptime", sent_body.find("1h 2m 5s") != string::npos ? "yes" : "no", "yes");
    check("metrics page shows status classes", sent_body.find("90 / 20 / 7 / 3") != string::npos ? "yes" : "no", "yes");
    check("metrics page shows busy workers / queue / free connections",
          sent_body.find("2 / " + to_string(WORKERS)) != string::npos && sent_body.find("5 / " + to_string(QUEUE_SIZE)) != string::npos &&
          sent_body.find("3 / " + to_string(DB_POOL)) != string::npos ? "yes" : "no", "yes");
    check("metrics page shows avg and p95", sent_body.find("12.35 ms") != string::npos && sent_body.find("80.50 ms") != string::npos ? "yes" : "no", "yes");
    check("metrics page shows rps, commit/rollback, dropped, 503, 408",
          sent_body.find("4.25") != string::npos && sent_body.find("6 / 1") != string::npos &&
          sent_body.find("<td>9</td>") != string::npos && sent_body.find("<td>2</td>") != string::npos &&
          sent_body.find("<td>1</td>") != string::npos ? "yes" : "no", "yes");
    check("metrics page is not cached", sent_extra.find("Cache-Control: no-store") != string::npos ? "yes" : "no", "yes");

    check("metrics.json from 127.0.0.1 -> 200", to_string(get("/metrics.json", "127.0.0.1")), "200");
    check("metrics.json content type", sent_type, "application/json");
    cout << "JSON " << sent_body << endl;                 // test_pages_helpers.py parses this with a real JSON parser

    check("metrics from another computer -> 403", to_string(get("/metrics", "192.168.1.20")), "403");
    check("metrics.json from another computer -> 403", to_string(get("/metrics.json", "10.0.0.5")), "403");
    check("403 page does not contain any numbers from the metrics",
          sent_body.find("120") == string::npos && sent_body.find("1h 2m") == string::npos ? "yes" : "no", "yes");
    check("a look-alike address is not local", to_string(get("/metrics", "127.0.0.10")), "403");
    check("empty address is not local", to_string(get("/metrics", "")), "403");
    check("POST /metrics from localhost -> 405", to_string(get("/metrics", "127.0.0.1", "POST")), "405");

    // ---- sold-out products: disabled button in the page, and the server refuses /cart/add ----
    string soldout = product_card({"7", "Gadget", "Audio", "499.00", "0", "Nice"}, "/products", true);
    check("stock 0: card has a disabled 'Out of stock' button", soldout.find("<button disabled>Out of stock</button>") != string::npos ? "yes" : "no", "yes");
    check("stock 0: card has NO add-to-cart form", soldout.find("/cart/add") == string::npos ? "yes" : "no", "yes");
    string instock = product_card({"7", "Gadget", "Audio", "1499.00", "5", "Nice"}, "/products", true);
    check("stock 5: card has the add-to-cart form and no disabled button",
          instock.find("action=\"/cart/add\"") != string::npos && instock.find("disabled") == string::npos ? "yes" : "no", "yes");
    check("stock 5: orange 'Only 5 left' badge", instock.find("Only 5 left") != string::npos ? "yes" : "no", "yes");
    check("price is shown with the rupee sign and a comma", instock.find("\xE2\x82\xB9" "1,499<") != string::npos ? "yes" : "no", "yes");
    string plenty = product_card({"7", "Gadget", "Audio", "99.50", "6", "Nice"}, "/products", true);
    check("stock 6: green 'In stock' badge", plenty.find("In stock") != string::npos && plenty.find("Only") == string::npos ? "yes" : "no", "yes");
    check("paise are kept when not .00", plenty.find("\xE2\x82\xB9" "99.50<") != string::npos ? "yes" : "no", "yes");
    string guest_card = product_card({"7", "Gadget", "Audio", "499.00", "5", "Nice"}, "/products", false);
    check("guest: 'Sign in to buy' opens the login popup and there is no add form",
          guest_card.find("Sign in to buy") != string::npos && guest_card.find("data-modal=\"loginModal\"") != string::npos
          && guest_card.find("/cart/add") == string::npos ? "yes" : "no", "yes");
    check("rupees(): 103350 -> 1,033.50 / 100 -> 1 digit / 1234567800 -> grouped",
          rupees(103350) + "|" + rupees(10000) + "|" + rupees(123456780000LL),
          string("\xE2\x82\xB9") + "1,033.50|" + "\xE2\x82\xB9" + "100|" + "\xE2\x82\xB9" + "1,234,567,800");
    check("status_class() keeps only lowercase letters", status_class("Logged out") + "|" + status_class("") + "|" + status_class("A-1"), "loggedout|other|a");

    fake_rows = {{"0"}};                                  // SELECT stock ... -> 0
    string msg, problem;
    cart_add_calls = 0;
    int st = cart_add_steps(nullptr, 3, "7", "1", msg, problem);
    check("server: /cart/add for a product with stock 0 is refused (problem 'out')", problem, "out");
    check("server: ... and nothing is added to the cart", to_string(cart_add_calls), "0");
    check("server: ... status 200 (redirect with a red message)", to_string(st), "200");
    fake_rows = {{"5"}};
    problem = "x"; cart_add_calls = 0;
    cart_add_steps(nullptr, 3, "7", "1", msg, problem);
    check("server: with stock 5 the add goes through", to_string(cart_add_calls), "1");

    cout << "FAILURES: " << fails << endl;
    return fails ? 1 : 0;
}
