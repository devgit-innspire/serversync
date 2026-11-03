// pages_render_check.cpp - runs EVERY page of src/pages.cpp through the real route() with fake database answers and
// prints what would be sent (status, headers, body), so test_templates.py can check the HTML.
// No database, no sockets, no db.conf. Run with the project folder as the current directory (it loads templates/).
#include "../src/pages.cpp"
#include <iostream>
#include <sstream>

// ---- capture what the page code sends ----
static int sent_status = 0;
static string sent_type, sent_body, sent_extra;
void send_response(SOCKET, int status, const string& type, const string& body, const string& extra) {
    sent_status = status; sent_type = type; sent_body = body; sent_extra = extra;
}
void log_tag(const string& tag, const string& msg) { cout << "LOG [" << tag << "] " << msg << endl; }
int thread_id() { return 3; }
static long long fake_now = 1000000;      // tests move the clock to check the category cache (60 s)
long long now_us() { return fake_now; }
string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// ---- fake database: rows are chosen by a piece of the SQL text ----
static vector<pair<string, vector<vector<string>>>> fake;       // (sql fragment, rows)
static int fake_exec_result = 1, fake_exec_code = 0;
static CheckoutResult fake_checkout;
static string fake_scalar = "52";
static char dummy_conn;
static void answer(const string& sql, vector<vector<string>>& rows) {
    rows.clear();
    // tests/test_mobile_layout.py sets MOBILE_FULL_CATS: every page then has the real 15 categories (long chip rows),
    // not the 2-3 names the other tests use. Without it nothing changes.
    if (getenv("MOBILE_FULL_CATS") && sql.find("FROM categories") != string::npos) {
        const char* names[] = {"Electronics", "Accessories", "Audio", "Mobile Phones", "Laptops", "Computers", "Cameras", "Gaming",
                               "Clothing", "Shoes", "Books", "Home & Kitchen", "Beauty & Personal Care", "Sports & Fitness", "Toys & Games"};
        for (int i = 0; i < 15; i++) rows.push_back({to_string(i + 1), names[i]});
        return;
    }
    for (auto& f : fake)
        if (sql.find(f.first) != string::npos) { rows = f.second; return; }
}
MYSQL* get_conn(int& id) { id = 2; return (MYSQL*)&dummy_conn; }
void put_conn(int) {}
bool db_select(const string& sql, const vector<string>&, vector<vector<string>>& rows, int*) { answer(sql, rows); return true; }
bool db_select_on(MYSQL*, const string& sql, const vector<string>&, vector<vector<string>>& rows, int*) { answer(sql, rows); return true; }
int db_exec(const string&, const vector<string>&, int* err) { if (err) *err = fake_exec_code; return fake_exec_result; }
int db_exec_on(MYSQL*, const string&, const vector<string>&, int* err) { if (err) *err = fake_exec_code; return fake_exec_result; }
bool db_scalar(const string&, string& out, int* conn) { out = fake_scalar; if (conn) *conn = 2; return true; }
CheckoutResult do_checkout(int, int) { return fake_checkout; }
// shipping addresses: the real ones are tested in test_addresses.py. The page code only needs these to link
static AddressResult fake_address;
static PasswordResult fake_password;        // what password_change() "returns" (the real one is tested in test_password.py)
static int password_calls = 0;
static string last_password_args;
PasswordResult password_change(int user, const string& cur, const string& pw, const string& keep) {
    password_calls++;
    last_password_args = to_string(user) + "|" + cur + "|" + pw + "|" + keep;
    cout << "PASSWORD_CALL " << last_password_args << endl;
    return fake_password;
}
string address_check(Address&) { return ""; }
AddressResult address_save(int, const Address&, bool) { return fake_address; }
AddressResult address_delete(int, int) { return fake_address; }
AddressResult address_set_default(int, int) { return fake_address; }
int find_active_cart(MYSQL*, int) { return 1; }
static OrderResult fake_order;              // what order_change() "returns" (the real one is tested in test_orders.py)
static int order_calls = 0;
static string last_order_args;
OrderResult order_change(int id, const string& to, int user, bool as_admin, const string& expected) {
    order_calls++;
    last_order_args = to_string(id) + "|" + to + "|" + to_string(user) + "|" + (as_admin ? "admin" : "customer") + "|" + expected;
    cout << "ORDER_CALL " << last_order_args << endl;
    return fake_order;
}
static RoleResult fake_role;                // what role_change() "returns" (the real one is tested in test_roles.py)
static int role_calls = 0;
RoleResult role_change(int admin, int user, const string& role) {
    role_calls++;
    cout << "ROLE_CALL " << admin << "|" << user << "|" << role << endl;
    return fake_role;
}
void admin_audit(int admin, const string& action, const string& details) {
    cout << "AUDIT " << admin << "|" << action << "|" << details << endl;
}
bool order_transition_ok(const string& from, const string& to) {      // same table as db.cpp: one step forward, or cancel
    static const char* FLOW[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
    int f = -1, t = -1;
    for (int i = 0; i < 5; i++) { if (from == FLOW[i]) f = i; if (to == FLOW[i]) t = i; }
    if (f < 0) return false;
    if (to == "CANCELLED") return f < 4;
    return t == f + 1;
}
string order_next_status(const string& status) {
    static const char* FLOW[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
    for (int i = 0; i < 4; i++) if (status == FLOW[i]) return FLOW[i + 1];
    return "";
}
int order_eta_seconds(const string&) { return 100; }
int get_active_cart(MYSQL*, int) { return 1; }
bool cart_add(MYSQL*, int, const string&, const string&) { return true; }
int cart_set_qty(MYSQL*, int, const string&, const string&) { return 1; }
int cart_remove(MYSQL*, int, const string&) { return 1; }
bool cart_items(MYSQL*, int, vector<vector<string>>& rows) { answer("cart_items", rows); return true; }
int cart_count(MYSQL*, int) { return 3; }
Metrics metrics_snapshot() {
    Metrics m;
    m.uptime_s = 3725; m.total = 120; m.c2xx = 90; m.c3xx = 20; m.c4xx = 7; m.c5xx = 3;
    m.busy_workers = 2; m.queue_length = 5; m.free_db_conns = 3; m.latency_samples = 100; m.avg_ms = 12.345; m.p95_ms = 80.5;
    return m;
}

static const string SESSION_A(64, 'a');

static Request mk(const string& method, const string& target, bool logged_in = true, map<string, string> form = {}) {
    Request r;
    r.method = method;
    r.target = target;
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != string::npos) {                                        // "a=1&b=2" (values are used as they are)
        string qs = target.substr(q + 1);
        size_t pos = 0;
        while (pos < qs.size()) {
            size_t amp = qs.find('&', pos);
            if (amp == string::npos) amp = qs.size();
            string pair = qs.substr(pos, amp - pos);
            size_t eq = pair.find('=');
            if (!pair.empty()) r.params[pair.substr(0, eq)] = eq == string::npos ? "" : pair.substr(eq + 1);
            pos = amp + 1;
        }
    }
    r.form = form;
    r.version = "HTTP/1.1";
    r.client_ip = "127.0.0.1";
    if (logged_in) {
        r.user_id = 5; r.role = "CUSTOMER"; r.user_name = "Ann <b>"; r.cart_count = 3; r.session_id = SESSION_A;
    }
    return r;
}

static void run(const string& name, Request req) {
    sent_status = 0; sent_type = ""; sent_body = ""; sent_extra = "";
    render_problems = 0; render_last_problem = "";
    fake_now += 61LL * 1000000LL;                                   // category cache (60 s) expires: each page asks the fake DB again
    int status = route(0, req);
    string extra_line;                                              // headers on ONE line: "A: 1 | B: 2 | "
    for (char c : sent_extra) {
        if (c == '\r') continue;
        if (c == '\n') extra_line += " | ";
        else extra_line += c;
    }
    sent_extra = extra_line;
    cout << "=== " << name << "\nSTATUS " << status << " SENT " << sent_status << "\nTYPE " << sent_type
         << "\nEXTRA " << (sent_extra.empty() ? "-" : sent_extra) << "\nPROBLEMS " << render_problems << " " << render_last_problem
         << "\nBODY\n" << sent_body << "\n=== END" << endl;
}

int main() {
    if (!load_templates("templates")) { cout << "TEMPLATES MISSING" << endl; return 2; }

    vector<vector<string>> cats = {{"1", "Audio"}, {"2", "Cameras"}};
    vector<string> p_in = {"7", "Gadget <b>x</b>", "Audio", "1499.00", "5", "Nice <i>thing</i> & more"};
    vector<string> p_out = {"8", "Sold Out Thing", "Cameras", "10.00", "0", "desc"};
    vector<string> p_ok = {"9", "Plenty", "Books", "99.50", "40", "plenty of stock"};

    // ---- public pages ----
    // home: 8 budget picks (what the budget query returns) and the category list
    vector<vector<string>> feat;
    const char* cat_names[] = {"Audio", "Cameras", "Books", "Gaming", "Shoes", "Toys", "Laptops", "Mobile Phones"};
    for (int i = 0; i < 8; i++)
        feat.push_back({to_string(20 + i), string("Featured ") + to_string(i + 1) + " <i>", cat_names[i], "99.00", "7", "Cheap thing <u>" + to_string(i + 1) + "</u>"});
    fake = {{"p.price <=", feat}, {"FROM categories", cats}, {"SELECT name, stock", {{"Featured 1 <i>", "7"}}}};
    run("home", mk("GET", "/", false));
    run("home_user", mk("GET", "/", true));
    run("home_added", mk("GET", "/?added=20", true));
    fake = {};
    run("home_nodb", mk("GET", "/", false));
    fake = {{"FROM categories", cats}, {"FROM products p JOIN", {p_in, p_out, p_ok}}};
    run("login_page_guest", mk("GET", "/login", false));
    run("404", mk("GET", "/nope", false));
    run("test", [] { Request r = mk("GET", "/test?a=1&b=<x>", false); r.headers["cookie"] = "session_id=SECRET"; r.headers["host"] = "h<1>"; return r; }());
    run("dbtest", mk("GET", "/dbtest", false));
    run("metrics", mk("GET", "/metrics", false));
    run("metrics_json", mk("GET", "/metrics.json", false));
    run("metrics_403", [] { Request r = mk("GET", "/metrics", false); r.client_ip = "10.0.0.2"; return r; }());

    // ---- products ----
    fake = {{"FROM categories", cats}, {"FROM products p JOIN", {p_in, p_out}}};
    run("products", mk("GET", "/products", false));
    run("products_user", mk("GET", "/products", true));
    run("products_max", mk("GET", "/products?max=1000", false));
    run("products_min_max", mk("GET", "/products?cat=1&q=a&min=100&max=500", false));
    run("products_bad_price", mk("GET", "/products?max=abc", false));
    run("products_cat_q", mk("GET", "/products?cat=1&q=lap", true));
    run("products_bad_cat", mk("GET", "/products?cat=abc", false));
    fake = {{"FROM categories", cats}, {"FROM products p JOIN", {}}};
    run("products_empty", mk("GET", "/products?q=zzz", false));
    fake = {{"FROM categories", cats}, {"FROM products p JOIN", {p_in}}, {"SELECT name, stock", {{"Gadget <b>x</b>", "5"}}}};
    run("products_added", mk("GET", "/products?added=7", true));
    run("products_added_max", mk("GET", "/products?added=7&cart_err=max", true));
    run("products_added_out", mk("GET", "/products?added=7&cart_err=out", true));
    run("products_added_stock", mk("GET", "/products?added=7&cart_err=stock", true));

    fake = {{"p.description", {{"7", "Gadget <b>x</b>", "Audio", "1499.00", "5", "Nice <i>thing</i> & more", "1"}}}, {"SELECT name, stock", {{"Gadget <b>x</b>", "5"}}}};
    run("product", mk("GET", "/product?id=7", true));
    run("product_added", mk("GET", "/product?id=7&added=7", true));
    fake = {{"p.description", {{"8", "Sold Out Thing", "Cameras", "10.00", "0", "desc", "2"}}}};
    run("product_soldout", mk("GET", "/product?id=8", false));
    fake = {};
    run("product_user_plenty", [] { fake = {{"p.description", {{"9", "Plenty", "Books", "99.50", "40", "plenty", "11"}}}}; return mk("GET", "/product?id=9", true); }());
    fake = {};
    run("product_404", mk("GET", "/product?id=99", false));
    run("product_400", mk("GET", "/product?id=abc", false));

    // ---- auth pages ----
    run("signup_get", mk("GET", "/signup", false));
    run("signup_invalid", mk("POST", "/signup", false, {{"name", "Bob <i>"}, {"email", "nope"}, {"password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    fake_exec_result = -1; fake_exec_code = 1062;
    run("signup_duplicate", mk("POST", "/signup", false, {{"name", "Bob"}, {"email", "b@x.com"}, {"password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    fake_exec_result = 1; fake_exec_code = 0;
    run("signup_ok", mk("POST", "/signup", false, {{"name", "Bob"}, {"email", "b@x.com"}, {"password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    run("login_get", mk("GET", "/login", false));
    run("login_registered", mk("GET", "/login?registered=1", false));
    fake = {};
    run("login_wrong", mk("POST", "/login", false, {{"email", "a<b>@x.com"}, {"password", "bad"}}));

    // ---- cart ----
    fake = {{"cart_items", {{"7", "Gadget <b>x</b>", "499.00", "2", "5"}, {"8", "Low Stock", "10.00", "3", "1"}, {"9", "Gone", "5.50", "1", "0"}}}};
    run("cart", mk("GET", "/cart", true));
    run("cart_guest", mk("GET", "/cart", false));
    run("cart_method", mk("GET", "/cart/add", true));
    fake = {};
    run("cart_empty", mk("GET", "/cart", true));
    fake = {{"SELECT stock", {{"5"}}}, {"SELECT quantity", {{"9"}}}};
    run("cart_add_max", mk("POST", "/cart/add", true, {{"product_id", "7"}, {"qty", "5"}, {"return", "/products?cat=1"}}));
    run("cart_add_bad", mk("POST", "/cart/add", true, {{"product_id", "x"}}));

    // ---- checkout / orders ----
    fake = {{"cart_items", {{"7", "Gadget <b>x</b>", "499.00", "2", "5"}, {"8", "Low Stock", "10.00", "3", "1"}}}};
    run("checkout_confirm", mk("GET", "/checkout", true));
    fake = {};
    run("checkout_confirm_empty", mk("GET", "/checkout", true));
    fake_checkout.status = 409; fake_checkout.message = "Only 2 of <b>Gadget</b> available";
    run("checkout_conflict", mk("POST", "/checkout", true, {{"address_id", "3"}}));
    fake_checkout.status = 503; fake_checkout.message = "The shop is busy right now.";
    run("checkout_busy", mk("POST", "/checkout", true, {{"address_id", "3"}}));
    fake_checkout.status = 200; fake_checkout.order_id = 42;
    run("checkout_ok", mk("POST", "/checkout", true, {{"address_id", "3"}}));

    fake = {{"FROM orders WHERE user_id", {{"7", "2026-10-24 10:00:00", "998.00", "PLACED"}, {"6", "2026-10-23 09:00:00", "10.00", "DELIVERED"}}}};
    run("orders", mk("GET", "/orders", true));
    fake = {};
    run("orders_empty", mk("GET", "/orders", true));
    fake = {{"FROM orders o LEFT JOIN payments", {{"7", "PLACED", "998.00", "2026-10-24 10:00:00", "COD", "PENDING", ""}}},
            {"FROM order_items", {{"7", "Gadget <b>x</b>", "2", "499.00"}}}};
    run("order", mk("GET", "/order?id=7", true));
    fake = {};
    run("order_404", mk("GET", "/order?id=99", true));
    run("order_400", mk("GET", "/order?id=abc", true));

    // ---- sessions ----
    fake = {{"FROM user_sessions WHERE user_id", {{SESSION_A, "2026-10-24 10:00:00", "2026-10-24 10:05:00", "", "1", "0", ""},
                                                  {string(64, 'b'), "2026-10-23 08:00:00", "2026-10-23 08:30:00", "2026-10-23 08:31:00", "0", "0", "LOGOUT"},
                                                  {string(64, 'c'), "2026-10-22 08:00:00", "2026-10-22 08:30:00", "2026-10-22 09:00:00", "0", "1", "EXPIRED"}}}};
    run("sessions", mk("GET", "/sessions", true));
    run("sessions_guest", mk("GET", "/sessions", false));
    // a product the admin hid while it sits in a cart
    fake = {{"cart_items", {{"7", "Gadget <b>x</b>", "499.00", "1", "5", "0"}}}};
    run("cart_hidden", mk("GET", "/cart", true));

    // ---- admin panel (A1 + A2): only ADMIN gets in; every /admin/* route is guarded ----
    auto as_admin = [](const string& method, const string& target, map<string, string> form = {}) {
        Request r = mk(method, target, true, form);
        r.role = "ADMIN"; r.user_name = "Boss <i>";
        return r;
    };
    fake = {{"SELECT COALESCE", {{"82672.00", "8", "7", "8", "52", "1", "8"}}},
            {"JOIN users u ON u.user_id = o.user_id", {{"14", "Aarav <b>Sharma</b>", "2026-10-13 19:29:39", "33892.00", "PLACED"},
                                                      {"13", "Sneha", "2026-10-13 19:27:14", "20498.00", "DELIVERED"},
                                                      {"12", "Raj", "2026-10-12 10:00:00", "10.50", "CANCELLED"}}},
            {"GROUP BY status", {{"PLACED", "7"}, {"DELIVERED", "1"}}},
            {"FROM products WHERE stock <=", {{"31", "Smart LED <b>Bulb</b>", "5"}, {"32", "Gone", "0"}}}};
    run("admin", as_admin("GET", "/admin"));
    run("admin_guest", mk("GET", "/admin", false));
    run("admin_customer", mk("GET", "/admin", true));
    run("admin_customer_deep", mk("GET", "/admin/products", true));
    run("admin_customer_post", mk("POST", "/admin/products/restock", true, {{"product_id", "1"}, {"delta", "+5"}}));
    run("admin_guest_post", mk("POST", "/admin/categories/delete", false, {{"category_id", "1"}}));
    run("admin_guest_deep", mk("GET", "/admin/nothing", false));
    run("admin_post", as_admin("POST", "/admin"));
    run("admin_users", as_admin("GET", "/admin/users"));
    run("admin_logs", as_admin("GET", "/admin/logs"));
    run("admin_404", as_admin("GET", "/admin/nothing"));
    run("admin_lookalike", as_admin("GET", "/administrator"));
    run("home_admin", [&] { Request r = as_admin("GET", "/"); return r; }());
    fake = {};
    run("admin_nodb", as_admin("GET", "/admin"));
    fake = {{"SELECT COALESCE", {{"0", "0", "0", "1", "52", "0", "1"}}}};
    run("admin_empty", as_admin("GET", "/admin"));

    // ---- A2: products list ----
    vector<vector<string>> cats2 = {{"1", "Audio"}, {"2", "Cameras"}};
    fake = {{"p.is_active FROM products p", {{"7", "Gadget <b>x</b>", "Audio", "1499.00", "5", "1"}, {"8", "Old Thing", "Cameras", "10.00", "0", "0"}}},
            {"FROM categories", cats2}};
    run("ap_list", as_admin("GET", "/admin/products"));
    run("ap_list_filtered", as_admin("GET", "/admin/products?q=gad&cat=1&hidden=1"));
    run("ap_list_badcat", as_admin("GET", "/admin/products?cat=abc"));
    run("ap_list_post", as_admin("POST", "/admin/products"));
    fake = {{"p.is_active FROM products p", {}}, {"FROM categories", cats2}};
    run("ap_list_empty", as_admin("GET", "/admin/products?q=zzz"));

    // ---- A2: add / edit form ----
    fake = {{"FROM categories", cats2}};
    run("ap_new_get", as_admin("GET", "/admin/products/new"));
    run("ap_new_put", as_admin("PUT", "/admin/products/new"));
    map<string, string> good = {{"name", "New <b>Item</b>"}, {"description", "A nice thing"}, {"price", "499.5"}, {"category_id", "1"}};
    fake_exec_result = 1; fake_exec_code = 0;
    run("ap_new_ok", as_admin("POST", "/admin/products/new", good));
    auto bad = [&](const string& key, const string& value) { map<string, string> f = good; f[key] = value; return f; };
    run("ap_new_noname", as_admin("POST", "/admin/products/new", bad("name", "   ")));
    run("ap_new_longname", as_admin("POST", "/admin/products/new", bad("name", string(101, 'n'))));
    run("ap_new_name100", as_admin("POST", "/admin/products/new", bad("name", string(100, 'n'))));
    run("ap_new_longdesc", as_admin("POST", "/admin/products/new", bad("description", string(256, 'd'))));
    run("ap_new_desc255", as_admin("POST", "/admin/products/new", bad("description", string(255, 'd'))));
    run("ap_new_price0", as_admin("POST", "/admin/products/new", bad("price", "0")));
    run("ap_new_priceneg", as_admin("POST", "/admin/products/new", bad("price", "-5")));
    run("ap_new_pricebig", as_admin("POST", "/admin/products/new", bad("price", "1000000.01")));
    run("ap_new_pricemax", as_admin("POST", "/admin/products/new", bad("price", "1000000")));
    run("ap_new_pricetext", as_admin("POST", "/admin/products/new", bad("price", "12abc")));
    run("ap_new_price3dec", as_admin("POST", "/admin/products/new", bad("price", "1.234")));
    run("ap_new_catnum", as_admin("POST", "/admin/products/new", bad("category_id", "x")));
    fake = {{"WHERE category_id = ?", {}}, {"FROM categories", cats2}};
    run("ap_new_catmissing", as_admin("POST", "/admin/products/new", bad("category_id", "99")));
    fake = {{"FROM categories", cats2}};
    fake_exec_result = -1; fake_exec_code = 0;
    run("ap_new_dberror", as_admin("POST", "/admin/products/new", good));
    fake_exec_result = 1;
    fake = {{"SELECT product_id, name, description, price, category_id FROM products", {{"7", "Gadget <b>x</b>", "Nice <i>d</i>", "1499.00", "2"}}},
            {"FROM categories", cats2}};
    run("ap_edit_get", as_admin("GET", "/admin/products/edit?id=7"));
    run("ap_edit_badid", as_admin("GET", "/admin/products/edit?id=abc"));
    fake = {{"FROM categories", cats2}};
    run("ap_edit_404", as_admin("GET", "/admin/products/edit?id=99"));
    map<string, string> egood = good; egood["product_id"] = "7";
    run("ap_edit_ok", as_admin("POST", "/admin/products/edit", egood));
    map<string, string> ebad = egood; ebad["price"] = "0";
    run("ap_edit_invalid", as_admin("POST", "/admin/products/edit", ebad));
    map<string, string> eid = egood; eid["product_id"] = "x";
    run("ap_edit_badpost_id", as_admin("POST", "/admin/products/edit", eid));
    fake_exec_result = 0;
    run("ap_edit_gone", as_admin("POST", "/admin/products/edit", egood));
    fake_exec_result = 1;

    // ---- A2: restock ----
    auto restock = [&](const string& delta, const string& ret = "") {
        map<string, string> f = {{"product_id", "7"}, {"delta", delta}};
        if (!ret.empty()) f["return"] = ret;
        return as_admin("POST", "/admin/products/restock", f);
    };
    run("ar_ok_plus", restock("+5"));
    run("ar_ok_minus", restock("-2"));
    run("ar_ok_plain", restock("007"));
    run("ar_back_admin", restock("+5", "/admin"));
    run("ar_back_list", restock("+5", "/admin/products?q=gad&hidden=1"));
    run("ar_back_old_flash", restock("+5", "/admin/products?err=stock_below"));
    run("ar_back_evil", restock("+5", "//evil.com"));
    run("ar_back_http", restock("+5", "http://evil.com/admin"));
    run("ar_back_store", restock("+5", "/products"));
    run("ar_back_lookalike", restock("+5", "/administrator"));
    run("ar_back_crlf", restock("+5", "/admin\r\nSet-Cookie: x=1"));
    run("ar_zero", restock("0"));
    run("ar_text", restock("abc"));
    run("ar_empty", restock(""));
    run("ar_float", restock("1.5"));
    run("ar_huge", restock("100001"));
    run("ar_get", as_admin("GET", "/admin/products/restock"));
    run("ar_badpid", as_admin("POST", "/admin/products/restock", {{"product_id", "x"}, {"delta", "+5"}}));
    fake_exec_result = 0;
    fake = {{"SELECT 1 FROM products", {{"1"}}}};
    run("ar_below", restock("-50"));
    fake = {{"SELECT 1 FROM products", {}}};
    run("ar_noproduct", restock("+5"));
    fake_exec_result = -1;
    run("ar_dberror", restock("+5"));
    fake_exec_result = 1;

    // ---- A2: toggle ----
    auto toggle = [&](const string& pid, const string& active) { return as_admin("POST", "/admin/products/toggle", {{"product_id", pid}, {"active", active}}); };
    run("at_hide", toggle("7", "0"));
    run("at_show", toggle("7", "1"));
    run("at_bad_active", toggle("7", "2"));
    run("at_bad_pid", toggle("x", "0"));
    run("at_get", as_admin("GET", "/admin/products/toggle"));
    fake_exec_result = 0;
    run("at_missing", toggle("99", "0"));
    fake_exec_result = 1;

    // ---- A2: categories ----
    fake = {{"LEFT JOIN products p", {{"1", "Audio <b>", "12"}, {"2", "Cameras", "0"}}}};
    run("ac_list", as_admin("GET", "/admin/categories"));
    fake = {{"LEFT JOIN products p", {}}};
    run("ac_list_empty", as_admin("GET", "/admin/categories"));
    fake = {};
    run("ac_add_ok", as_admin("POST", "/admin/categories/add", {{"name", "Garden"}}));
    run("ac_add_empty", as_admin("POST", "/admin/categories/add", {{"name", "  "}}));
    run("ac_add_long", as_admin("POST", "/admin/categories/add", {{"name", string(51, 'c')}}));
    run("ac_add_50", as_admin("POST", "/admin/categories/add", {{"name", string(50, 'c')}}));
    run("ac_add_get", as_admin("GET", "/admin/categories/add"));
    fake_exec_result = -1; fake_exec_code = 1062;
    run("ac_add_dup", as_admin("POST", "/admin/categories/add", {{"name", "Audio"}}));
    run("ac_rename_dup", as_admin("POST", "/admin/categories/rename", {{"category_id", "2"}, {"name", "Audio"}}));
    fake_exec_code = 1451;
    run("ac_delete_fk", as_admin("POST", "/admin/categories/delete", {{"category_id", "1"}}));
    fake_exec_code = 0;
    fake_exec_result = -1;
    run("ac_add_dberror", as_admin("POST", "/admin/categories/add", {{"name", "Garden"}}));
    fake_exec_result = 1;
    run("ac_rename_ok", as_admin("POST", "/admin/categories/rename", {{"category_id", "2"}, {"name", "Photo"}}));
    run("ac_rename_badid", as_admin("POST", "/admin/categories/rename", {{"category_id", "x"}, {"name", "Photo"}}));
    run("ac_rename_noname", as_admin("POST", "/admin/categories/rename", {{"category_id", "2"}, {"name", ""}}));
    run("ac_delete_ok", as_admin("POST", "/admin/categories/delete", {{"category_id", "2"}}));
    run("ac_delete_badid", as_admin("POST", "/admin/categories/delete", {{"category_id", "x"}}));
    fake_exec_result = 0;
    run("ac_delete_missing", as_admin("POST", "/admin/categories/delete", {{"category_id", "99"}}));
    fake_exec_result = 1;

    // ---- A3: admin orders ----
    vector<vector<string>> olist = {{"14", "Aarav <b>Sharma</b>", "aarav@x.com", "2026-10-13 19:29:39", "3", "33892.00", "PLACED", "PENDING"},
                                    {"13", "Sneha", "sneha@x.com", "2026-10-13 19:27:14", "1", "20498.00", "DELIVERED", "PAID"},
                                    {"12", "Raj", "raj@x.com", "2026-10-12 10:00:00", "2", "10.50", "CANCELLED", "CANCELLED"},
                                    {"11", "Mia", "mia@x.com", "2026-10-11 10:00:00", "1", "5.00", "SHIPPED", "-"}};
    fake = {{"FROM orders o JOIN users u", olist}};
    run("ao_list", as_admin("GET", "/admin/orders"));
    run("ao_list_filtered", as_admin("GET", "/admin/orders?status=SHIPPED&q=a%b%20c"));
    run("ao_list_badstatus", as_admin("GET", "/admin/orders?status=HACKED"));
    run("ao_list_badpage", as_admin("GET", "/admin/orders?page=0"));
    run("ao_list_badpage2", as_admin("GET", "/admin/orders?page=abc"));
    run("ao_list_post", as_admin("POST", "/admin/orders"));
    vector<vector<string>> big;                                    // 21 rows: a 21st row means "there is a next page"
    for (int i = 0; i < 21; i++) big.push_back({to_string(100 - i), "Cust", "c@x.com", "2026-10-01 10:00:00", "1", "1.00", "PLACED", "PENDING"});
    fake = {{"FROM orders o JOIN users u", big}};
    run("ao_page1", as_admin("GET", "/admin/orders?status=PLACED&q=c@x.com"));
    run("ao_page2", as_admin("GET", "/admin/orders?page=2"));
    fake = {{"FROM orders o JOIN users u", {}}};
    run("ao_empty", as_admin("GET", "/admin/orders?q=nobody"));
    fake = {};
    run("ao_dberror", as_admin("GET", "/admin/orders"));

    auto order_head = [&](const string& status, const string& pay) {
        return vector<vector<string>>{{"7", status, "998.00", "2026-10-24 10:00:00", "Ann <b>", "ann@x.com", "COD", pay, pay == "PAID" ? "2026-10-25 09:00:00" : ""}};
    };
    vector<vector<string>> oitems = {{"1", "Gadget <b>x</b>", "2", "499.00"}};
    for (const char* st : {"PLACED", "SHIPPED", "DELIVERED", "CANCELLED"}) {
        string pay = string(st) == "DELIVERED" ? "PAID" : string(st) == "CANCELLED" ? "CANCELLED" : "PENDING";
        fake = {{"SELECT o.order_id, o.status, o.total, o.created_at, u.name", order_head(st, pay)}, {"FROM order_items oi", oitems}};
        run(string("ao_detail_") + st, as_admin("GET", "/admin/order?id=7"));
    }
    fake = {{"SELECT o.order_id, o.status, o.total, o.created_at, u.name", {}}};
    run("ao_detail_404", as_admin("GET", "/admin/order?id=99"));
    run("ao_detail_badid", as_admin("GET", "/admin/order?id=abc"));
    run("ao_detail_post", as_admin("POST", "/admin/order?id=7"));
    run("ao_detail_guest", mk("GET", "/admin/order?id=7", false));
    run("ao_detail_customer", mk("GET", "/admin/order?id=7", true));

    order_calls = 0;
    auto set_status = [&](const string& id, const string& to, const string& from) {
        map<string, string> f = {{"order_id", id}, {"to", to}};
        if (!from.empty()) f["from"] = from;
        return as_admin("POST", "/admin/order/status", f);
    };
    fake_order = OrderResult(); fake_order.status = 200; fake_order.message = ""; fake_order.from = "PLACED";
    run("as_ship", set_status("7", "SHIPPED", "PLACED"));
    fake_order.from = "SHIPPED";
    run("as_deliver", set_status("7", "DELIVERED", "SHIPPED"));
    run("as_cancel", set_status("7", "CANCELLED", ""));
    fake_order.status = 400; fake_order.message = "Not allowed: an order cannot go from DELIVERED to SHIPPED.";
    run("as_notallowed", set_status("7", "SHIPPED", "DELIVERED"));
    fake_order.status = 404; fake_order.message = "Order not found";
    run("as_404", set_status("99", "SHIPPED", ""));
    fake_order.status = 409; fake_order.message = "This order was changed by someone else (now SHIPPED). Reload the page and check it.";
    run("as_409", set_status("7", "CANCELLED", "PLACED"));
    fake_order.status = 503; fake_order.message = "The shop is busy right now. Please try again in a moment.";
    run("as_503", set_status("7", "SHIPPED", ""));
    fake_order.status = 500; fake_order.message = "Database error";
    run("as_500", set_status("7", "SHIPPED", ""));
    int calls_before_bad = order_calls;
    run("as_bad_to", set_status("7", "PLACED", ""));
    run("as_bad_to2", set_status("7", "PENDING", ""));
    run("as_bad_id", set_status("x", "SHIPPED", ""));
    run("as_bad_from", set_status("7", "SHIPPED", "NOPE"));
    run("as_get", as_admin("GET", "/admin/order/status"));
    run("as_customer", mk("POST", "/admin/order/status", true, {{"order_id", "7"}, {"to", "SHIPPED"}}));
    run("as_guest", mk("POST", "/admin/order/status", false, {{"order_id", "7"}, {"to", "SHIPPED"}}));
    cout << "ORDER_CALLS_BAD " << (order_calls - calls_before_bad - 0) << endl;      // customer / guest / bad input must not reach order_change

    // ---- A3: customer side ----
    auto cust_head = [&](const string& status) {
        return vector<vector<string>>{{"7", status, "998.00", "2026-10-24 10:00:00", "COD", "PENDING", ""}};
    };
    for (const char* st : {"PLACED", "SHIPPED", "DELIVERED", "CANCELLED"}) {
        fake = {{"FROM orders o LEFT JOIN payments", cust_head(st)}, {"FROM order_items", {{"1", "Gadget <b>x</b>", "2", "499.00"}}}};
        run(string("oc_") + st, mk("GET", "/order?id=7", true));
    }
    run("oc_flash", mk("GET", "/order?id=7&ok=cancelled", true));
    order_calls = 0;
    fake_order = OrderResult(); fake_order.status = 200; fake_order.message = ""; fake_order.from = "PLACED";
    run("oc_cancel_ok", mk("POST", "/order/cancel", true, {{"order_id", "7"}}));
    cout << "CUSTOMER_CALL " << last_order_args << endl;
    fake_order.status = 400; fake_order.message = "Not allowed: an order cannot go from SHIPPED to CANCELLED.";
    run("oc_cancel_400", mk("POST", "/order/cancel", true, {{"order_id", "7"}}));
    fake_order.status = 404; fake_order.message = "Order not found";
    run("oc_cancel_404", mk("POST", "/order/cancel", true, {{"order_id", "7"}}));
    fake_order.status = 503; fake_order.message = "The shop is busy right now. Please try again in a moment.";
    run("oc_cancel_503", mk("POST", "/order/cancel", true, {{"order_id", "7"}}));
    fake_order.status = 500; fake_order.message = "Database error";
    run("oc_cancel_500", mk("POST", "/order/cancel", true, {{"order_id", "7"}}));
    int calls_before = order_calls;
    run("oc_cancel_guest", mk("POST", "/order/cancel", false, {{"order_id", "7"}}));
    run("oc_cancel_get", mk("GET", "/order/cancel", true));
    run("oc_cancel_badid", mk("POST", "/order/cancel", true, {{"order_id", "x"}}));
    cout << "CUSTOMER_BAD_CALLS " << (order_calls - calls_before) << endl;

    // ---- A4: users ----
    vector<vector<string>> ulist = {{"5", "Ann <b>", "ann@x.com", "ADMIN", "2026-01-01 10:00:00", "3", "1"},
                                    {"6", "Bob", "bob@x.com", "CUSTOMER", "2026-02-01 10:00:00", "12", "2"},
                                    {"7", "Cy", "cy@x.com", "CUSTOMER", "2026-03-01 10:00:00", "0", "0"},
                                    {"8", "Dee", "dee@x.com", "ADMIN", "2026-04-01 10:00:00", "1", "0"}};
    fake = {{"FROM users u WHERE 1=1", ulist}};
    run("au_list", as_admin("GET", "/admin/users"));
    run("au_search", as_admin("GET", "/admin/users?q=a%b_c"));
    run("au_badpage", as_admin("GET", "/admin/users?page=0"));
    run("au_post", as_admin("POST", "/admin/users"));
    vector<vector<string>> ubig;
    for (int i = 0; i < 21; i++) ubig.push_back({to_string(100 + i), "U", "u@x.com", "2026-01-01 10:00:00", "0", "0", "0"});
    fake = {{"FROM users u WHERE 1=1", ubig}};
    run("au_page1", as_admin("GET", "/admin/users?q=u"));
    run("au_page2", as_admin("GET", "/admin/users?page=2"));
    fake = {{"FROM users u WHERE 1=1", {}}};
    run("au_empty", as_admin("GET", "/admin/users?q=zzz"));

    role_calls = 0;
    auto set_role = [&](const string& id, const string& role) { return as_admin("POST", "/admin/users/role", {{"user_id", id}, {"role", role}}); };
    fake_role = RoleResult(); fake_role.status = 200; fake_role.code = ""; fake_role.from = "CUSTOMER";
    run("au_role_ok", set_role("6", "ADMIN"));
    for (const char* code : {"own_role", "last_admin", "same_role", "not_admin"}) {
        fake_role = RoleResult(); fake_role.status = 400; fake_role.code = code;
        run(string("au_role_") + code, set_role("6", "ADMIN"));
    }
    fake_role = RoleResult(); fake_role.status = 404; fake_role.code = "no_user";
    run("au_role_no_user", set_role("99", "ADMIN"));
    fake_role = RoleResult(); fake_role.status = 503; fake_role.code = "busy";
    run("au_role_busy", set_role("6", "ADMIN"));
    fake_role = RoleResult(); fake_role.status = 500; fake_role.code = "db_error";
    run("au_role_500", set_role("6", "ADMIN"));
    int role_calls_before = role_calls;
    run("au_role_bad_role", set_role("6", "SUPERUSER"));
    run("au_role_bad_id", set_role("x", "ADMIN"));
    run("au_role_get", as_admin("GET", "/admin/users/role"));
    run("au_role_customer", mk("POST", "/admin/users/role", true, {{"user_id", "6"}, {"role", "ADMIN"}}));
    run("au_role_guest", mk("POST", "/admin/users/role", false, {{"user_id", "6"}, {"role", "ADMIN"}}));
    cout << "ROLE_BAD_CALLS " << (role_calls - role_calls_before) << endl;

    auto logout_all = [&](const string& id) { return as_admin("POST", "/admin/users/logout-all", {{"user_id", id}}); };
    fake = {};
    fake_exec_result = 3; fake_exec_code = 0;
    run("au_la_ok", logout_all("6"));
    fake_exec_result = 0;
    run("au_la_none", logout_all("7"));
    run("au_la_own", logout_all("5"));
    run("au_la_bad", logout_all("x"));
    run("au_la_get", as_admin("GET", "/admin/users/logout-all"));
    fake_exec_result = -1;
    run("au_la_dberror", logout_all("6"));
    fake_exec_result = 1;

    // ---- A4: sessions ----
    const string sA(64, 'a'), sB(64, 'b'), sC(64, 'c'), sD(64, 'd'), sE(64, 'e');
    vector<vector<string>> slist = {{sA, "Ann <b>", "ann@x.com", "2026-10-24 10:00:00", "2026-10-24 10:05:00", "2026-10-24 10:35:00", "1", "0", ""},
                                    {sB, "Bob", "bob@x.com", "2026-10-24 09:00:00", "2026-10-24 09:30:00", "2026-10-24 10:30:00", "1", "0", ""},
                                    {sC, "Cy", "cy@x.com", "2026-10-23 09:00:00", "2026-10-23 09:30:00", "2026-10-23 10:30:00", "0", "0", "FORCED"},
                                    {sD, "Dee", "dee@x.com", "2026-10-22 09:00:00", "2026-10-22 09:30:00", "2026-10-22 10:30:00", "0", "0", "LOGOUT"},
                                    {sE, "Eve", "eve@x.com", "2026-10-21 09:00:00", "2026-10-21 09:30:00", "2026-10-21 10:30:00", "1", "1", ""}};
    fake = {{"FROM user_sessions s JOIN users u", slist}};
    run("sx_list", as_admin("GET", "/admin/sessions"));
    run("sx_all", as_admin("GET", "/admin/sessions?show=all"));
    run("sx_badshow", as_admin("GET", "/admin/sessions?show=everything"));
    run("sx_badpage", as_admin("GET", "/admin/sessions?page=x"));
    run("sx_post", as_admin("POST", "/admin/sessions"));
    vector<vector<string>> sbig;
    for (int i = 0; i < 21; i++) sbig.push_back({string(64, 'f'), "U", "u@x.com", "2026-10-24 10:00:00", "2026-10-24 10:00:00", "2026-10-24 11:00:00", "1", "0", ""});
    fake = {{"FROM user_sessions s JOIN users u", sbig}};
    run("sx_page1", as_admin("GET", "/admin/sessions?show=all"));
    fake = {{"FROM user_sessions s JOIN users u", {}}};
    run("sx_empty", as_admin("GET", "/admin/sessions"));

    auto end_session = [&](const string& sid) { return as_admin("POST", "/admin/sessions/end", {{"session_id", sid}}); };
    fake = {{"WHERE session_id LIKE ?", {{sB}}}};
    fake_exec_result = 1;
    run("sx_end_ok", end_session("bbbbbbbb"));
    run("sx_end_full", end_session(sB));
    fake = {{"WHERE session_id LIKE ?", {{sA}}}};
    run("sx_end_own", end_session("aaaaaaaa"));
    fake = {{"WHERE session_id LIKE ?", {}}};
    run("sx_end_gone", end_session("bbbbbbbb"));
    fake = {{"WHERE session_id LIKE ?", {{sB}, {string(64, 'b')}}}};
    run("sx_end_ambiguous", end_session("bbbbbbbb"));
    fake = {{"WHERE session_id LIKE ?", {{sB}}}};
    fake_exec_result = 0;
    run("sx_end_race", end_session("bbbbbbbb"));
    fake_exec_result = -1;
    run("sx_end_dberror", end_session("bbbbbbbb"));
    fake_exec_result = 1;
    for (const char* bad : {"zzzzzzzz", "bbbbbbb", "BBBBBBBB", "", "bbbbbbbbb", "b'; DROP TABLE users;--"})
        run(string("sx_end_bad_") + to_string(strlen(bad)) + (bad[0] == 'B' ? "u" : bad[0] == 'z' ? "z" : "x"), end_session(bad));
    run("sx_end_get", as_admin("GET", "/admin/sessions/end"));
    run("sx_end_customer", mk("POST", "/admin/sessions/end", true, {{"session_id", "bbbbbbbb"}}));
    run("sx_end_guest", mk("POST", "/admin/sessions/end", false, {{"session_id", "bbbbbbbb"}}));

    // customer's own /sessions page: "Ended by admin"
    fake = {{"FROM user_sessions WHERE user_id", {{sA, "2026-10-24 10:00:00", "2026-10-24 10:05:00", "", "1", "0", ""},
                                                  {sC, "2026-10-23 08:00:00", "2026-10-23 08:30:00", "2026-10-23 08:31:00", "0", "0", "FORCED"}}}};
    run("sx_customer_page", mk("GET", "/sessions", true));

    // ---- A4: logs ----
    vector<vector<string>> loglist = {{"2026-10-24 10:00:01", "GET", "/products?q=<b>", "200", "1.250", "0.100", "2", "Ann <b> (#5)"},
                                      {"2026-10-24 10:00:02", "POST", "/checkout", "503", "0.500", "20.000", "3", "-"},
                                      {"2026-10-24 10:00:03", "GET", "/nope", "404", "0.200", "0.000", "2", "-"},
                                      {"2026-10-24 10:00:04", "GET", "/redirect", "302", "0.200", "0.000", "4", "-"}};
    vector<vector<string>> workers = {{"2", "50", "1.5"}, {"3", "25", "3.126"}, {"4", "0", "0"}};
    vector<vector<string>> actions = {{"2026-10-24 10:10:00", "Boss <i>", "ORDER_STATUS", "order 7 PLACED->SHIPPED"},
                                      {"2026-10-24 10:09:00", "Boss <i>", "PRODUCT_RESTOCK", "product 3 by +5 <b>"}};
    fake = {{"FROM request_logs l LEFT JOIN users", loglist}, {"GROUP BY worker_id", workers}, {"FROM admin_actions a", actions}};
    run("lg_list", as_admin("GET", "/admin/logs"));
    run("lg_filtered", as_admin("GET", "/admin/logs?worker=2&cls=5xx&path=%25a_b"));
    run("lg_refresh", as_admin("GET", "/admin/logs?refresh=1&cls=2xx"));
    run("lg_badworker", as_admin("GET", "/admin/logs?worker=abc"));
    run("lg_badcls", as_admin("GET", "/admin/logs?cls=6xx"));
    run("lg_post", as_admin("POST", "/admin/logs"));
    fake = {{"FROM request_logs l LEFT JOIN users", {}}, {"GROUP BY worker_id", {}}, {"FROM admin_actions a", {}}};
    run("lg_empty", as_admin("GET", "/admin/logs"));
    run("lg_guest", mk("GET", "/admin/logs", false));
    run("lg_customer", mk("GET", "/admin/logs", true));

    // flash messages: fixed texts chosen by a code in the URL
    fake = {{"LEFT JOIN products p", {}}, {"p.is_active FROM products p", {}}, {"FROM categories", cats2}};
    run("ap_flash_ok", as_admin("GET", "/admin/products?ok=restocked"));
    run("ap_flash_err", as_admin("GET", "/admin/products?err=stock_below&ok=restocked"));
    run("ap_flash_cat", as_admin("GET", "/admin/categories?err=cat_exists"));
    run("ap_flash_fk", as_admin("GET", "/admin/categories?err=cat_has_products"));
    run("ap_flash_junk", as_admin("GET", "/admin/products?ok=<script>alert(1)</script>&err=nope"));

    // ---- extra pages with realistic data for the mobile layout check (tests/test_mobile_layout.py) ----
    vector<vector<string>> addr_rows = {
        {"1", "Asha", "Kumar", "9876543210", "Flat 402, Sunrise Residency, 12 Rajpur Road Extension", "Opposite the old clock tower", "Near City Mall", "Dehradun", "Uttarakhand", "248001", "HOME", "1"},
        {"2", "Asha", "Kumar", "9876543210", "Graphic Era Hill University, Bell Road", "", "", "Dehradun", "Uttarakhand", "248002", "WORK", "0"}};
    fake = {{"FROM addresses a", addr_rows}};
    run("mb_addresses", mk("GET", "/addresses", true));
    fake = {};
    run("mb_addresses_empty", mk("GET", "/addresses", true));
    fake = {{"FROM states", {{"1", "Andhra Pradesh"}, {"26", "Uttar Pradesh"}, {"27", "Uttarakhand"}}}};
    run("mb_address_new", mk("GET", "/addresses/new", true));
    run("mb_address_new_checkout", mk("GET", "/addresses/new?return=checkout", true));
    fake = {{"FROM states", {{"1", "Andhra Pradesh"}, {"27", "Uttarakhand"}}},
            {"FROM addresses WHERE address_id", {{"Asha", "Kumar", "9876543210", "Flat 402", "Rajpur Road", "Near City Mall", "Dehradun", "27", "248001", "HOME"}}}};
    run("mb_address_edit", mk("GET", "/addresses/edit?id=1", true));
    fake = {{"cart_items", {{"7", "Gadget <b>x</b>", "499.00", "2", "5"}, {"8", "Low Stock", "10.00", "3", "1"}}}, {"FROM addresses a", addr_rows}};
    run("mb_checkout", mk("GET", "/checkout", true));
    fake = {{"cart_items", {{"7", "Gadget <b>x</b>", "499.00", "2", "5"}}}};
    run("mb_checkout_no_address", mk("GET", "/checkout", true));
    vector<vector<string>> shipped_hist = {{"PLACED", "24 Oct, 10:00:00", "CUSTOMER"}, {"PACKED", "24 Oct, 10:00:31", "SYSTEM"}, {"SHIPPED", "24 Oct, 10:01:20", "SYSTEM"}};
    vector<vector<string>> oaddr = {{"Asha", "Kumar", "9876543210", "Flat 402, Sunrise Residency", "Rajpur Road", "Near City Mall", "Dehradun", "Uttarakhand", "248001", "HOME"}};
    fake = {{"FROM orders o LEFT JOIN payments", {{"7", "SHIPPED", "998.00", "2026-10-24 10:00:00", "COD", "PENDING", ""}}},
            {"FROM order_items", {{"7", "Gadget <b>x</b>", "2", "499.00"}, {"8", "A very long product name that has to wrap on a narrow phone screen", "1", "10.00"}}},
            {"FROM order_status_history", shipped_hist}, {"next_step_at IS NOT NULL", {{"24 Oct, 10:02:30"}}}, {"FROM order_addresses", oaddr}};
    run("mb_order_shipped", mk("GET", "/order?id=7", true));
    fake = {{"FROM orders o LEFT JOIN payments", {{"7", "CANCELLED", "998.00", "2026-10-24 10:00:00", "COD", "CANCELLED", ""}}},
            {"FROM order_items", {{"7", "Gadget <b>x</b>", "2", "499.00"}}},
            {"FROM order_status_history", {{"PLACED", "24 Oct, 10:00:00", "CUSTOMER"}, {"CANCELLED", "24 Oct, 10:00:40", "CUSTOMER"}}}, {"FROM order_addresses", oaddr}};
    run("mb_order_cancelled", mk("GET", "/order?id=7", true));
    fake = {{"SELECT o.order_id, o.status, o.total, o.created_at, u.name", order_head("SHIPPED", "PENDING")}, {"FROM order_items oi", oitems},
            {"FROM order_status_history", shipped_hist}, {"FROM order_addresses", oaddr}};
    run("mb_admin_order", as_admin("GET", "/admin/order?id=7"));
    fake = {{"FROM categories", cats}};
    run("mb_login_guest", mk("GET", "/login", false));
    run("mb_metrics_proxy", [] { Request r = mk("GET", "/metrics", false); r.headers["x-forwarded-for"] = "203.0.113.9"; return r; }());
    run("mb_metrics_proxy_host", [] { Request r = mk("GET", "/metrics.json", false); r.headers["x-forwarded-host"] = "abc.ngrok.app"; return r; }());

    // ---- password policy (tests/test_password.py) ----
    {
        struct PwCase { const char* label; string pw, name, email, old; };
        vector<PwCase> pwc = {
            {"good", "Zx9#kLm2Qw", "Ann Lee", "ann.lee@x.com", ""},
            {"short", "Ab1@xyz", "Ann Lee", "ann@x.com", ""},
            {"len64", "Aa1@" + string(60, 'x'), "Ann Lee", "ann@x.com", ""},
            {"len65", "Aa1@" + string(61, 'x'), "Ann Lee", "ann@x.com", ""},
            {"space", "Zx9# kLm2Qw", "Ann Lee", "ann@x.com", ""},
            {"tab", "Zx9#\tkLm2Qw", "Ann Lee", "ann@x.com", ""},
            {"no_upper", "zx9#klm2qw", "Ann Lee", "ann@x.com", ""},
            {"no_lower", "ZX9#KLM2QW", "Ann Lee", "ann@x.com", ""},
            {"no_digit", "Zxy#kLmpQw", "Ann Lee", "ann@x.com", ""},
            {"no_special", "Zx9kLm2QwE", "Ann Lee", "ann@x.com", ""},
            {"underscore_is_special", "Zx9_kLm2Qw", "Ann Lee", "ann@x.com", ""},
            {"has_name_word", "Xx#ANN2025q", "Ann Lee", "bob@x.com", ""},
            {"has_full_name", "Xx#annlee9Z", "Ann Lee", "bob@x.com", ""},
            {"has_email_local", "Xx#Priya99!", "Ann", "priya@x.com", ""},
            {"short_name_ignored", "Xx#al12345Q", "Al", "al@x.com", ""},
            {"common_Password@123", "Password@123", "Ann Lee", "bob@x.com", ""},
            {"common_ADMIN@123", "ADMIN@123", "Ann Lee", "bob@x.com", ""},
            {"common_qwerty", "qwerty@123", "Ann Lee", "bob@x.com", ""},
            {"same_as_old", "Zx9#kLm2Qw", "Ann Lee", "bob@x.com", "Zx9#kLm2Qw"},
            {"different_from_old", "Zx9#kLm2Qw", "Ann Lee", "bob@x.com", "Old@pass1"},
            {"old_case_differs_is_different", "Zx9#kLm2Qw", "Ann Lee", "bob@x.com", "zx9#klm2qw"},
            {"non_ascii_counts_as_chars", "Zx9#é€₹kLm", "Ann Lee", "bob@x.com", ""},
            {"empty", "", "Ann Lee", "bob@x.com", ""},
        };
        for (auto& c : pwc) {
            vector<string> bad = check_password(c.pw, c.name, c.email, c.old);
            string joined;
            for (auto& b : bad) joined += (joined.empty() ? "" : " || ") + b;
            cout << "PWCHECK " << c.label << " " << bad.size() << " " << joined << endl;
        }
    }
    fake = {{"FROM categories", cats}};
    run("pw_signup_get", mk("GET", "/signup", false));
    run("pw_signup_weak", mk("POST", "/signup", false, {{"name", "Bob <i>"}, {"email", "bob@x.com"}, {"password", "weak"}, {"confirm_password", "weak"}}));
    run("pw_signup_mismatch", mk("POST", "/signup", false, {{"name", "Bob"}, {"email", "bob@x.com"}, {"password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qx"}}));
    run("pw_signup_no_confirm", mk("POST", "/signup", false, {{"name", "Bob"}, {"email", "bob@x.com"}, {"password", "Zx9#kLm2Qw"}}));
    run("pw_signup_bad_email_weak", mk("POST", "/signup", false, {{"name", "Bob"}, {"email", "nope"}, {"password", "weak"}, {"confirm_password", "weak"}}));
    run("pw_signup_contains_name", mk("POST", "/signup", false, {{"name", "Bobby"}, {"email", "b@x.com"}, {"password", "Bobby#2025"}, {"confirm_password", "Bobby#2025"}}));
    run("pw_signup_ok", mk("POST", "/signup", false, {{"name", "Bob"}, {"email", "bob@x.com"}, {"password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));

    // login: the password rules are NOT checked at login (an old weak password still works); wrong password = the same old message
    fake = {{"FROM users WHERE email", {{"5"}}}};
    run("pw_login_weak", mk("POST", "/login", false, {{"email", "ann@x.com"}, {"password", "Pass@1234"}}));
    run("pw_login_strong", mk("POST", "/login", false, {{"email", "ann@x.com"}, {"password", "Zx9#kLm2Qw"}}));
    fake = {{"FROM users WHERE email", {}}};
    run("pw_login_wrong", mk("POST", "/login", false, {{"email", "ann@x.com"}, {"password", "weak"}}));

    // /account/password
    fake = {{"SELECT email FROM users", {{"ann@x.com"}}}, {"FROM categories", cats}};
    run("pw_change_get", mk("GET", "/account/password", true));
    run("pw_change_get_ok", mk("GET", "/account/password?ok=1", true));
        run("pw_change_guest", mk("GET", "/account/password", false));
    run("pw_change_put", mk("PUT", "/account/password", true));
    int pw_calls_before = password_calls;
    run("pw_change_weak", mk("POST", "/account/password", true, {{"current_password", "Old@pass1"}, {"new_password", "weak"}, {"confirm_password", "weak"}}));
    run("pw_change_mismatch", mk("POST", "/account/password", true, {{"current_password", "Old@pass1"}, {"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Other"}}));
    run("pw_change_same", mk("POST", "/account/password", true, {{"current_password", "Zx9#kLm2Qw"}, {"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    run("pw_change_no_current", mk("POST", "/account/password", true, {{"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    run("pw_change_guest_post", mk("POST", "/account/password", false, {{"current_password", "a"}, {"new_password", "b"}, {"confirm_password", "b"}}));
    cout << "PW_BAD_CALLS " << (password_calls - pw_calls_before) << endl;       // none of the above may reach password_change
    fake_password.status = 403; fake_password.message = "Current password is incorrect.";
    run("pw_change_wrong_current", mk("POST", "/account/password", true, {{"current_password", "Nope@pass1"}, {"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    fake_password.status = 503; fake_password.message = "The shop is busy right now. Please try again in a moment.";
    run("pw_change_busy", mk("POST", "/account/password", true, {{"current_password", "Old@pass1"}, {"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    fake_password.status = 500; fake_password.message = "Database error";
    run("pw_change_dberror", mk("POST", "/account/password", true, {{"current_password", "Old@pass1"}, {"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));
    fake_password.status = 200; fake_password.message = ""; fake_password.ended = 2;
    run("pw_change_ok", mk("POST", "/account/password", true, {{"current_password", "Old@pass1"}, {"new_password", "Zx9#kLm2Qw"}, {"confirm_password", "Zx9#kLm2Qw"}}));

    // admin Users page: no password button any more
    fake = {{"FROM users u WHERE 1=1", ulist}};
    run("pw_admin_users", as_admin("GET", "/admin/users"));
    run("pw_admin_require_gone", as_admin("POST", "/admin/users/require-pw", {{"user_id", "6"}}));
    run("pw_not_forced_products", mk("GET", "/products", true));
    fake = {{"SELECT email FROM users", {{"ann@x.com"}}}, {"FROM categories", cats}};
    run("pw_change_server_error", mk("POST", "/account/password", true, {{"current_password", "Old@pass1"}, {"new_password", "alllowercase1"}, {"confirm_password", "alllowercase1"}}));

    // category cache: a successful change clears it, so the store's navbar re-reads the categories at once
    fake = {{"FROM categories", {{"1", "Audio"}}}};
    run("cache_prime", mk("GET", "/products", false));
    fake_now -= 61LL * 1000000LL;                               // run() will move the clock again: undo so the cache is still fresh
    fake = {{"FROM categories", {{"1", "Renamed"}}}};
    cout << "CACHE_DEMO " << (get_categories(true)[0][1]) << endl;      // still the cached (old) name
    clear_category_cache();
    cout << "CACHE_CLEARED " << (get_categories(true)[0][1]) << endl;   // refreshed from the (fake) database at once
    return 0;
}
