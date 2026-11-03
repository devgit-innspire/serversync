// pages.cpp - routes, sessions (signup/login/logout), templates ka render() aur pages ka data
// Owner: Ashutosh
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>      // BCryptGenRandom: OS ka secure random number generator
#endif
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include "common.h"
using namespace std;

const string HTML = "text/html; charset=utf-8";
const string COOKIE_NAME = "session_id";

// Session ka naya expire time (SQL ka tukda): abhi + SESSION_MINUTES.
// SESSION_MINUTES hamara constant hai, user input nahi, isliye ise SQL me jodna theek hai
const string EXPIRES_SQL = "NOW() + INTERVAL " + to_string(SESSION_MINUTES) + " MINUTE";

// Rupee ka nishaan (UTF-8 ke 3 bytes). Alag const isliye ki "\xB9" ke baad koi digit likhne par wo hex ka hissa ban jaata hai
const string RUPEE = "\xE2\x82\xB9";

// categories ki list: navbar, home aur products page sab ke liye. id ke order me (Electronics, Accessories, Audio ...)
const string CATEGORIES_SQL = "SELECT category_id, category_name FROM categories ORDER BY category_id";

// Home page ke "Budget picks": BUDGET_MAX (common.h ka constant, user input nahi) tak ke in-stock products, sasta pehle.
// Columns wahi jo product_card ko chahiye: id, name, category, price, stock, description
const string BUDGET_SQL =
    "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description "
    "FROM products p JOIN categories c ON c.category_id = p.category_id "
    "WHERE p.price <= " + to_string(BUDGET_MAX) + " AND p.stock > 0 AND p.is_active = TRUE ORDER BY p.price, p.product_id LIMIT 10";

// user ka text HTML me daalne se pehle escape karo (warna script chal sakti hai)
static string html_escape(const string& s) {
    string out;
    for (char c : s) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else out += c;
    }
    return out;
}

// ---- templates: templates/*.html startup par ek baar memory me, request ke waqt disk nahi padhte ----

static map<string, string> templates;           // naam ("layout") -> poora .html text
static mutex render_problem_lock;               // neeche ki ginti ko bachata hai (workers ek saath render karte hain)
static int render_problems = 0;                 // kisi template ko koi key nahi mili / template hi nahi mila (tests dekhte hain)
static string render_last_problem;

// jin templates ke bina code chal hi nahi sakta. Inme se koi na mile to server start nahi hota
static const char* REQUIRED_TEMPLATES[] = {
    "layout", "nav", "search_form", "search_option", "account_guest", "account_user", "cart_link", "subnav", "subnav_link",
    "flash", "flash_text", "error", "detail_text", "plain", "heading", "empty_state", "meta_refresh",
    "home", "hero_create", "hero_orders", "home_categories", "category_tile", "home_budget",
    "login", "signup", "products", "side_cat", "hidden_input", "no_products",
    "product_card", "thumb_photo", "thumb_emoji", "stock_in", "stock_low", "soldout", "signin_buy", "add_form_card", "add_form_page", "product",
    "stock_warn", "cart_row", "cart", "checkout_row", "checkout", "checkout_failed",
    "order_row", "orders", "order_item_row", "order_payment", "order", "session_row", "sessions",
    "admin_layout", "admin_link", "admin_nav_link", "admin_card", "admin_order_row", "admin_status_count", "admin_low_row",
    "admin_low", "admin_none", "admin_none_row", "admin_message", "admin_dashboard", "admin_restock_form",
    "admin_users", "admin_user_row", "admin_user_me", "admin_role_form", "admin_logout_all_form", "admin_sessions", "admin_session_row",
    "admin_end_session_form", "admin_tab", "admin_logs", "admin_worker_row", "admin_log_row", "admin_activity_row", "admin_orders", "admin_orders_row", "admin_pager", "admin_pager_link", "admin_order", "admin_order_item_row",
    "admin_order_action", "admin_order_buttons", "order_cancel", "timeline", "timeline_step", "timeline_eta", "admin_history_row",
    "error_list", "error_list_item", "pw_field", "pw_rules", "pw_rule_item", "change_password",
    "drawer", "drawer_user", "drawer_admin", "drawer_guest", "tabbar",
    "address_extra", "address_text", "address_none", "address_default_badge", "address_default_form", "address_card", "addresses",
    "addresses_empty", "address_form", "checkout_address", "checkout_no_address", "checkout_place", "checkout_payment", "order_address", "admin_products", "admin_product_row", "admin_product_form", "admin_categories", "admin_category_row", "metric_row", "metrics", "test_item", "test", "dbtest",
    "flash_added", "flash_max", "flash_out", "flash_stock", "modals", "scripts"};

// dir ki saari *.html files (naam = file ka naam .html ke bina). Return: zaroori sab mil gaye?
bool load_templates(const string& dir) {
    templates.clear();
    error_code ec;
    if (filesystem::is_directory(dir, ec)) {
        for (auto& entry : filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".html") continue;
            ifstream f(entry.path(), ios::binary);
            string text((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
            // file ke aakhir ki khali lines hata do: chhote tukde (partials) jodne par extra khali jagah na bane
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            templates[entry.path().stem().string()] = text;
        }
    }

    bool ok = true;
    for (const char* name : REQUIRED_TEMPLATES) {
        if (templates.find(name) == templates.end()) {
            log_tag("PAGE", "missing template: " + dir + "/" + name + ".html");
            ok = false;
        }
    }
    if (ok) log_tag("PAGE", "templates loaded: " + to_string(templates.size()) + " from '" + dir + "'");
    return ok;
}

// cache busting: /static/style.css?v={{ver}}. Server start ka time: har naye start par browser nayi css/js maangta hai.
// Har template me yeh apne aap milta hai (static handler ?query ko ignore karta hai)
static const string STATIC_VER = to_string((long long)time(nullptr));

// values me key ki value. Na mile to "" aur ek problem note hoti hai (galti pakadne ke liye)
static string template_value(const string& tname, const map<string, string>& values, const string& key_text) {
    string key = trim(key_text);
    auto it = values.find(key);
    if (it != values.end()) return it->second;
    if (key == "ver") return STATIC_VER;

    render_problem_lock.lock();
    render_problems++;
    render_last_problem = "template '" + tname + "' needs the value '" + key + "'";
    render_problem_lock.unlock();
    log_tag("PAGE", "template '" + tname + "' needs the value '" + key + "'");
    return "";
}

// Template me do tarah ke nishaan:
//   {{key}}   -> value HTML-escape hokar (user/DB ka text hamesha isse)
//   {{{key}}} -> value jaisi hai waisi (sirf wo HTML jo hamne khud banaya, jaise cards ki list)
// Ek hi baar padhta hai: jo value andar daali gayi usme "{{...}}" ho to wo dobara expand nahi hota
string render(const string& name, const map<string, string>& values) {
    auto t = templates.find(name);
    if (t == templates.end()) {
        render_problem_lock.lock();
        render_problems++;
        render_last_problem = "template '" + name + "' not found";
        render_problem_lock.unlock();
        log_tag("PAGE", "template '" + name + "' not found");
        return "";
    }

    const string& src = t->second;
    string out;
    size_t i = 0;
    while (i < src.size()) {
        if (src.compare(i, 3, "{{{") == 0) {
            size_t end = src.find("}}}", i + 3);
            if (end != string::npos) {
                out += template_value(name, values, src.substr(i + 3, end - i - 3));    // raw
                i = end + 3;
                continue;
            }
        } else if (src.compare(i, 2, "{{") == 0) {
            size_t end = src.find("}}", i + 2);
            if (end != string::npos) {
                out += html_escape(template_value(name, values, src.substr(i + 2, end - i - 2)));
                i = end + 2;
                continue;
            }
        }
        out += src[i];
        i++;
    }
    return out;
}

// ---- page helpers ----

static string session_cookie(const string& value, int max_age);   // neeche "sessions" section me
static vector<vector<string>> get_categories(bool allow_db);        // neeche, helpers me
static string category_emoji(const string& name);
static string rupees(long long cents);
static map<string, string> pw_parts(bool change_form, const vector<string>* failed);   // neeche, password section me

// Har reply ke headers me cookie ka kaam (agar reply khud koi Set-Cookie nahi bhej raha, jaise login/logout):
//  - session expire mila: cookie hatao (Max-Age=0)
//  - user logged in hai: cookie dobara bhejo, naye Max-Age ke saath (neeche "idle timeout" dekho)
static string with_cookie_fix(const Request& req, const string& extra) {
    if (extra.find("Set-Cookie") != string::npos) return extra;
    if (req.clear_cookie) return extra + session_cookie("", 0);
    if (req.refresh_cookie) return extra + session_cookie(req.session_id, SESSION_MINUTES * 60);
    return extra;
}

// navigation bar: login hai to naam + cart badge + Orders + Sessions + Logout, warna Login + Signup
// (logout POST hai, link nahi: koi dusri site <img src=/logout> se logout na kara sake)
static string nav_bar(const Request& req, bool allow_db) {
    vector<vector<string>> cats = get_categories(allow_db);

    // search form: category <select> (ab chuni hui category selected) + text box. GET /products?cat=&q=
    string cur_cat, cur_q;
    auto c = req.params.find("cat");
    if (c != req.params.end()) cur_cat = c->second;
    auto qi = req.params.find("q");
    if (qi != req.params.end()) cur_q = qi->second;

    string options = render("search_option", {{"value", ""}, {"label", "All"}, {"selected", cur_cat.empty() ? "selected" : ""}});
    string links;
    for (auto& cat : cats) {
        options += render("search_option", {{"value", cat[0]}, {"label", cat[1]}, {"selected", cat[0] == cur_cat ? "selected" : ""}});
        links += render("subnav_link", {{"id", cat[0]}, {"emoji", category_emoji(cat[1])}, {"name", cat[1]}});
    }

    // account: guest ko "Sign in / Sign up" (popup kholte hain), logged-in ko naam + Orders + My Sessions + Logout
    string account = req.user_id > 0 ? render("account_user", {{"name", req.user_name}, {"admin", req.role == "ADMIN" ? render("admin_nav_link", {}) : ""}})
                                     : render("account_guest", {});
    string cart = render("cart_link", {{"cart_count", to_string(req.cart_count)},
                                       {"cart_unit", req.cart_count == 1 ? "item" : "items"}});
    string subnav = render("subnav", {{"links", links}, {"budget", rupees((long long)BUDGET_MAX * 100)},
                                      {"budget_max", to_string(BUDGET_MAX)}});

    // mobile: ☰ drawer (account links) aur neeche ki tab bar. Desktop par css inhe chhupa deti hai (display: none).
    // Drawer :target se bhi khulta hai, isliye JavaScript ke bina bhi links milte hain
    string items = req.user_id > 0 ? render("drawer_user", {{"name", req.user_name}, {"admin", req.role == "ADMIN" ? render("drawer_admin", {}) : ""}})
                                   : render("drawer_guest", {});
    const string& p = req.path;
    auto on = [&p](bool yes) { return string(yes ? "on" : ""); };
    string tabbar = render("tabbar", {{"cart_count", to_string(req.cart_count)}, {"home_on", on(p == "/")},
                                      {"cats_on", on(p == "/products" || p == "/product")},
                                      {"orders_on", on(p == "/orders" || p == "/order")},
                                      {"acct_on", on(p == "/addresses" || p == "/sessions")},
                                      {"cart_on", on(p == "/cart" || p == "/checkout")}});
    return render("nav", {{"search", render("search_form", {{"options", options}, {"q", cur_q}})},
                          {"account", account}, {"cart", cart}, {"subnav", subnav},
                          {"drawer", render("drawer", {{"items", items}})}, {"tabbar", tabbar}});
}

// message ka box (kind = "ok" hara, "err" laal). message_html hamara banaya HTML hona chahiye (user text escape ho chuka)
static string flash_box(const string& kind, const string& message_html) {
    return render("flash", {{"kind", kind}, {"message", message_html}});
}

// wahi, par text hai (user/DB ka bhi ho sakta hai): engine ise escape karta hai
static string flash_text(const string& kind, const string& text) {
    return render("flash_text", {{"kind", kind}, {"message", text}});
}

// poora HTML page: layout.html me title, nav, flash message aur content bharo.
// head_extra = <head> me extra tag (jaise /metrics ka meta refresh). flash = upar ka message box (ya "")
// interactive = login/signup popups (sirf logged-out user ko) aur app.js. /metrics isse band rakhta hai (usme JavaScript nahi)
// allow_db = navbar ki category list ke liye DB use kar sakte hain? server.cpp ke error pages aur /metrics "false" dete hain:
// accept thread ya metrics ko DB ka intezaar kabhi nahi karna chahiye (sirf memory ki purani list chalegi)
static string page(const Request& req, const string& title, const string& content, const string& head_extra = "",
                   const string& flash = "", bool interactive = true, bool allow_db = true) {
    string modals = (interactive && req.user_id == 0) ? render("modals", pw_parts(false, nullptr)) : "";
    string scripts = interactive ? render("scripts", {}) : "";
    return render("layout", {{"title", title}, {"head_extra", head_extra}, {"nav", nav_bar(req, allow_db)},
                             {"flash", flash}, {"content", content}, {"modals", modals}, {"scripts", scripts}});
}

// page bhejo aur status return karo (route me ek line me kaam)
static int send_page(SOCKET client, const Request& req, int status, const string& title, const string& content,
                     const string& extra = "", const string& flash = "") {
    send_response(client, status, HTML, page(req, title, content, "", flash), with_cookie_fix(req, extra));
    return status;
}

static string error_content(const string& msg, const string& detail_html = "") {
    return render("error", {{"message", msg}, {"detail", detail_html}});
}

static int error_page(SOCKET client, const Request& req, int status, const string& msg) {
    return send_page(client, req, status, msg, error_content(msg));
}

// server.cpp ke apne error replies (400 / 408 / 503) ke liye: bina login ke poora page
string render_error_page(const string& message) {
    Request anonymous;
    return page(anonymous, message, error_content(message), "", "", true, false);   // DB nahi: accept thread ko rukna nahi hai
}

// 302 redirect: Post/Redirect/Get. location sirf hamare fixed paths hain (user input nahi)
static int redirect(SOCKET client, const Request& req, const string& location, const string& extra = "") {
    send_response(client, 302, HTML, render("plain", {{"message", "Redirecting..."}}),
                  "Location: " + location + "\r\n" + with_cookie_fix(req, extra));
    return 302;
}

// galat method par 405 + Allow header
static int method_not_allowed(SOCKET client, const Request& req, const string& allow) {
    send_response(client, 405, HTML, page(req, "Method Not Allowed", error_content("Method Not Allowed")),
                  with_cookie_fix(req, "Allow: " + allow + "\r\n"));
    return 405;
}

// "499.00" -> 49900 (paise). Paise ko integer me jodte hain taaki float ki galti na ho (price DECIMAL(10,2) hai)
static long long to_cents(const string& price) {
    long long v = 0;
    for (char c : price)
        if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
    return v;
}

// 149900 -> "₹1,499", 103350 -> "₹1,033.50" (hazaar par comma; paise tabhi dikhte hain jab 00 na ho)
static string rupees(long long cents) {
    string whole = to_string(cents / 100);
    string grouped;
    for (size_t i = 0; i < whole.size(); i++) {
        if (i > 0 && (whole.size() - i) % 3 == 0) grouped += ',';
        grouped += whole[i];
    }
    long long paise = cents % 100;
    string out = RUPEE + grouped;
    if (paise != 0) out += "." + string(paise < 10 ? "0" : "") + to_string(paise);
    return out;
}

// DB ki price string ("1499.00") seedhe ₹ text me
static string rupees_text(const string& price) {
    return rupees(to_cents(price));
}

// Har category ka ek emoji (card ke rangeen dibbe, navbar aur category tiles me). Table me na ho to 🛍️
struct CategoryEmoji {
    const char* name;
    const char* emoji;
};
static const CategoryEmoji CATEGORY_EMOJIS[] = {
    {"Electronics", "💡"}, {"Accessories", "🔋"}, {"Audio", "🎧"}, {"Mobile Phones", "📱"}, {"Laptops", "💻"},
    {"Computers", "🖥️"}, {"Cameras", "📷"}, {"Gaming", "🎮"}, {"Clothing", "👕"}, {"Shoes", "👟"}, {"Books", "📚"},
    {"Home & Kitchen", "🍳"}, {"Beauty & Personal Care", "💄"}, {"Sports & Fitness", "🏋️"}, {"Toys & Games", "🧸"}};

static string category_emoji(const string& name) {
    for (const CategoryEmoji& c : CATEGORY_EMOJIS)
        if (name == c.name) return c.emoji;
    return "🛍️";
}

// category ke naam se ek rang (0-359): same category = hamesha same rang (card ka halka rangeen dibba)
static int thumb_hue(const string& category) {
    unsigned int sum = 0;
    for (unsigned char c : category) sum = sum * 31 + c;
    return (int)(sum % 360);
}

// status text se css class ka naam: sirf chhote akshar ("Logged out" -> "loggedout"). DB ka text hai, isliye saaf karke
static string status_class(const string& status) {
    string out;
    for (unsigned char c : status)
        if (isalpha(c)) out += (char)tolower(c);
    return out.empty() ? "other" : out;
}

// order status ka dikhne wala naam (DB me PLACED, OUT_FOR_DELIVERY ... rehta hai). Unknown status jaisa hai waisa
static string status_label(const string& status) {
    if (status == "PLACED") return "Ordered";
    if (status == "PACKED") return "Packed";
    if (status == "SHIPPED") return "Shipped";
    if (status == "OUT_FOR_DELIVERY") return "Out for delivery";
    if (status == "DELIVERED") return "Delivered";
    if (status == "CANCELLED") return "Cancelled";
    return status;
}

// stock badge: "In stock" (hara) ya LOW_STOCK tak "Only N left" (narangi). Stock 0 par kuch nahi: wahan disabled button hota hai
static string stock_html(const string& stock) {
    int n = atoi(stock.c_str());
    if (n <= 0) return "";
    if (n <= LOW_STOCK) return render("stock_low", {{"stock", to_string(n)}});
    return render("stock_in", {{"stock", to_string(n)}});      // badge sirf "In stock" dikhata hai, tooltip me poori ginti
}

// "123" jaisa number hai? (sirf digits, 1 se 9 tak)
static bool is_number(const string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// LIKE me % _ aur ! ko apna normal arth na karne do (hum ESCAPE '!' use karte hain)
static string escape_like(const string& s) {
    string out;
    for (char c : s) {
        if (c == '!' || c == '%' || c == '_') out += '!';
        out += c;
    }
    return out;
}

// params me key ki value, na ho to ""
static string param(const Request& req, const string& key) {
    auto it = req.params.find(key);
    return it == req.params.end() ? "" : it->second;
}

// form me key ki value, na ho to ""
static string field(const Request& req, const string& key) {
    auto it = req.form.find(key);
    return it == req.form.end() ? "" : it->second;
}

// result ki har row me kam se kam n columns hain? (rows khali ho to bhi true; rows[0] se pehle
// rows.empty() alag se dekho). Isse rows[i][j] kabhi range ke bahar nahi jayega
static bool cols_ok(const vector<vector<string>>& rows, size_t n) {
    for (auto& r : rows)
        if (r.size() < n) {
            log_tag("PAGE", "unexpected query result shape (expected " + to_string(n) + " columns)");
            return false;
        }
    return true;
}

// ---- categories ka cache ----
// Navbar har page par category list maangta hai. Har baar DB jaana bekaar (categories kam badalti hain), isliye 60 second
// memory me. Kai workers ek saath padhte hain, isliye mutex. DB fail ho to purani list chalti rehti hai.
static mutex cat_lock;
static vector<vector<string>> cat_cache;        // (id, naam)
static long long cat_loaded_us = 0;             // 0 = abhi tak load nahi hua
const long long CAT_CACHE_US = 60LL * 1000000LL;

static vector<vector<string>> get_categories(bool allow_db) {
    cat_lock.lock();
    vector<vector<string>> copy = cat_cache;
    bool fresh = cat_loaded_us != 0 && now_us() - cat_loaded_us < CAT_CACHE_US;
    cat_lock.unlock();
    if (fresh || !allow_db) return copy;

    vector<vector<string>> rows;                 // DB ka kaam mutex ke bina
    if (!db_select(CATEGORIES_SQL, {}, rows) || !cols_ok(rows, 2)) return copy;
    cat_lock.lock();
    cat_cache = rows;
    cat_loaded_us = now_us();
    cat_lock.unlock();
    return rows;
}

// ---- sessions ----

// 32 random bytes -> 64 hex characters. rand() nahi: wo guess ho sakta hai, BCryptGenRandom nahi
static bool new_session_id(string& out) {
    unsigned char raw[32];
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
#else
    // Linux ka secure random: /dev/urandom (kernel ka CSPRNG)
    ifstream rnd("/dev/urandom", ios::binary);
    if (!rnd.read((char*)raw, sizeof(raw))) return false;
#endif
    const char* hex = "0123456789abcdef";
    out = "";
    for (int i = 0; i < 32; i++) {
        out += hex[raw[i] >> 4];
        out += hex[raw[i] & 15];
    }
    return true;
}

// cookie ki value sach me 64 hex chars hai? (warna DB tak jaane ki zaroorat nahi)
static bool looks_like_session_id(const string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// cookie header: HttpOnly = JavaScript nahi padh sakti, SameSite=Lax = dusri site se POST me nahi jaati,
// Path=/ = poori site par, Max-Age = kitne second baad browser hata de (0 = abhi hata do)
static string session_cookie(const string& value, int max_age) {
    return "Set-Cookie: " + COOKIE_NAME + "=" + value + "; HttpOnly; SameSite=Lax; Path=/; Max-Age=" +
           to_string(max_age) + "\r\n";
}

// session dhundo, DB me check karo, req.user_id / user_name / cart_count bharo.
// Connection m diya hua hai: yeh function jitni bhi queries chalaye sab usi par (get/put bahar wrapper karta hai)
static void load_user_on(MYSQL* m, Request& req, const string& sid) {
    // is_active = 1 wale session lo; expire hua ya nahi, wo SQL se hi (DB ki ghadi se) pooch lete hain
    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT s.user_id, u.name, s.expires_at <= NOW(), u.role FROM user_sessions s "
                         "JOIN users u ON u.user_id = s.user_id WHERE s.session_id = ? AND s.is_active = 1",
                      {sid}, rows))
        return;                         // DB error: anonymous maano
    if (rows.empty()) return;           // session nahi mila / pehle hi band ho chuka (logout ya expire)
    if (!cols_ok(rows, 4)) return;      // result ka shape galat: anonymous maano

    if (rows[0][2] == "1") {
        // session expire ho gaya: DB me band karo (logout_time = expires_at), user ko logged out maano,
        // aur reply me cookie hatao. "AND is_active = 1": cleanup thread ne pehle band kiya ho to kuch nahi badlega
        db_exec_on(m, "UPDATE user_sessions SET is_active = 0, logout_time = expires_at, end_reason = 'EXPIRED' "
                      "WHERE session_id = ? AND is_active = 1", {sid});
        req.clear_cookie = true;
        log_tag("PAGE", "session of user " + rows[0][0] + " expired");
        return;
    }

    // IDLE TIMEOUT (sliding expiry): session tab tak chalta hai jab tak user SESSION_MINUTES minute tak
    // kuch na kare. Har request par expire time wapas "abhi + SESSION_MINUTES" kar dete hain, to jab tak
    // user active hai logout nahi hota; sirf chup rehne par (idle) expire hota hai. Ek hi UPDATE me
    // last_activity aur expires_at dono badalte hain, taaki dono hamesha saath rahein.
    // WHERE me "is_active = 1 AND expires_at > NOW()": SELECT aur UPDATE ke beech session expire/logout ho gaya ho
    // to use zinda nahi karte (0 rows match hongi)
    int n = db_exec_on(m, "UPDATE user_sessions SET last_activity = NOW(), expires_at = " + EXPIRES_SQL +
                          " WHERE session_id = ? AND is_active = 1 AND expires_at > NOW()", {sid});
    if (n == 0) {                       // session abhi abhi khatam ho gaya: logged out maano, cookie hatao
        req.clear_cookie = true;
        return;
    }

    req.user_id = atoi(rows[0][0].c_str());
    req.user_name = rows[0][1];
    req.role = rows[0][3] == "ADMIN" ? "ADMIN" : "CUSTOMER";    // kuch aur (ya khali) = sabse kam haq
    req.session_id = sid;

    // Browser ki cookie aur DB ka expiry saath rahein, isliye har reply me cookie dobara bhejte hain (naya Max-Age).
    // n < 0 (DB error): DB ka expiry badla nahi, to cookie bhi nahi badlte (user is baar andar aane diya)
    req.refresh_cookie = (n > 0);

    // nav bar ke "Cart (N items)" ke liye; error ho to 0 dikha do
    int count = cart_count(m, req.user_id);
    req.cart_count = count > 0 ? count : 0;
}

// har request par: cookie se session dhundo, req.user_id / user_name bharo.
// Is request ki saari session queries ek hi connection par: get_conn shuru me, put_conn aakhir me
static void load_user(Request& req) {
    auto it = req.cookies.find(COOKIE_NAME);
    if (it == req.cookies.end() || !looks_like_session_id(it->second)) return;   // cookie nahi: DB tak jaana nahi

    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return;           // connection nahi mila: anonymous maano
    load_user_on(m, req, it->second);
    put_conn(id);                       // load_user_on me early return ho ya na ho, connection yahin wapas
}

// ---- forms ----


// ---- password policy ----
// Niyam sirf server par asli hain (yahi check_password). Form ki checklist (app.js) sirf live dikhati hai, aur DB ke CHECK me yeh
// niyam nahi: wo typed password par lagte hain, aur baad me hash karne par DB me typed shakal rahegi hi nahi.
// Aam (common) passwords, chhote akshar me (match case-insensitive hota hai)
static const char* COMMON_PW[] = {"password@123", "admin@123", "qwerty@123", "welcome@123", "abcd@1234", "password@1234", "password1!",
                                  "passw0rd!", "p@ssw0rd", "p@ssword1", "admin@1234", "admin@12345", "qwerty@1234", "welcome@1",
                                  "welcome@1234", "test@1234", "india@123", "india@1234", "pass@1234", "abc@12345", "abc@1234",
                                  "letmein@123", "changeme@123", "iloveyou@123", "user@1234", "master@123", "monkey@123",
                                  "dragon@123", "login@1234", "hello@1234"};

static string lower_ascii(string t) {
    for (char& c : t) c = (char)tolower((unsigned char)c);
    return t;
}

// characters ki ginti (bytes nahi): UTF-8 ka continuation byte (10xxxxxx) nahi ginte
static size_t pw_chars(const string& t) {
    size_t n = 0;
    for (unsigned char c : t)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

// Fail hue niyam: (niyam ka key, message), niyam ke order me (yahi order form ki checklist ka hai). Key form ki list me wahi niyam dikhane
// ke kaam aata hai (data-rule). Upper / lower / digit / special sirf ASCII se gine jaate hain (special = chhapne layak, letter/digit nahi)
static vector<pair<string, string>> pw_failures(const string& pw, const string& name, const string& email, const string& old_pw) {
    vector<pair<string, string>> bad;

    size_t len = pw_chars(pw);
    if (len < (size_t)PW_MIN_LEN || len > (size_t)PW_MAX_LEN)
        bad.push_back({"len", "Password must be " + to_string(PW_MIN_LEN) + " to " + to_string(PW_MAX_LEN) + " characters long."});

    bool space = false, upper = false, lower = false, digit = false, special = false;
    for (unsigned char c : pw) {
        if (c <= 32 || c == 127) space = true;                  // space, tab, newline aur baaki control characters
        else if (c >= 'A' && c <= 'Z') upper = true;
        else if (c >= 'a' && c <= 'z') lower = true;
        else if (c >= '0' && c <= '9') digit = true;
        else if (c < 127) special = true;                       // 33..126 me letter/digit nahi
    }
    if (space) bad.push_back({"space", "Password cannot contain spaces."});
    if (!upper) bad.push_back({"upper", "Password needs at least one uppercase letter (A-Z)."});
    if (!lower) bad.push_back({"lower", "Password needs at least one lowercase letter (a-z)."});
    if (!digit) bad.push_back({"digit", "Password needs at least one number (0-9)."});
    if (!special) bad.push_back({"special", "Password needs at least one special character (like @ # $ ! %)."});

    // naam ya email ka hissa (case-insensitive). Sirf 3+ characters ke tukde (warna "al" jaisa chhota naam har password rok deta):
    // poora naam (space hata kar), naam ka har shabd, aur email me @ se pehle ka hissa
    string low = lower_ascii(pw);
    vector<string> bits;
    string joined, word;
    for (char c : lower_ascii(name) + " ") {
        if (c == ' ' || c == '\t') {
            if (word.size() >= 3) bits.push_back(word);
            joined += word;
            word = "";
        } else {
            word += c;
        }
    }
    if (joined.size() >= 3) bits.push_back(joined);
    string local = lower_ascii(email.substr(0, email.find('@')));
    if (local.size() >= 3) bits.push_back(local);
    for (auto& b : bits)
        if (low.find(b) != string::npos) {
            bad.push_back({"personal", "Password cannot contain your name or the first part of your email."});
            break;
        }

    for (const char* c : COMMON_PW)
        if (low == c) {
            bad.push_back({"common", "That password is too common. Please choose a less guessable one."});
            break;
        }

    if (!old_pw.empty() && pw == old_pw) bad.push_back({"different", "New password must be different from your current password."});
    return bad;
}

// Fail hue niyamon ke messages (khali list = password theek)
vector<string> check_password(const string& pw, const string& name, const string& email, const string& old_pw) {
    vector<string> msgs;
    for (auto& f : pw_failures(pw, name, email, old_pw)) msgs.push_back(f.second);
    return msgs;
}

// password ka input (aankh wala button JavaScript chalne par hi dikhta hai). kind: new / confirm / old (app.js yahi dekhta hai)
static string pw_field_html(const string& name, const string& kind, const string& autocomplete, const string& placeholder, int max_len) {
    return render("pw_field", {{"name", name}, {"kind", kind}, {"auto", autocomplete}, {"ph", placeholder}, {"max", to_string(max_len)}});
}

// niyamon ki checklist. Pehle chhupi rehti hai (css: .pw-rules { display: none }); app.js password field par focus / likhne par dikhati hai, aur
// likhte waqt ✓ / ✗ karti hai. Aam passwords ki list data-common me: app.js ko wahi list milti hai jo server ke paas hai.
// failed != nullptr: server ne galti pakdi. Tab list khuli aati hai aur sirf fail hue niyam dikhte hain (baaki .pw-passed se chhupe: app.js
// user ke likhna shuru karte hi poori list live dikhati hai). JavaScript ke bina yahi fail hue niyam dikhte hain
struct PwRule {
    const char* key;
    string label;
};

static string pw_rules_html(bool with_different, const vector<string>* failed) {
    string common;
    for (const char* c : COMMON_PW) common += (common.empty() ? "" : "|") + string(c);

    vector<PwRule> rules = {{"len", to_string(PW_MIN_LEN) + " to " + to_string(PW_MAX_LEN) + " characters"},
                            {"space", "No spaces"},
                            {"upper", "One uppercase letter (A-Z)"},
                            {"lower", "One lowercase letter (a-z)"},
                            {"digit", "One number (0-9)"},
                            {"special", "One special character (like @ # $ ! %)"},
                            {"personal", "Does not contain your name or email name"},
                            {"common", "Not a common password"}};
    if (with_different) rules.push_back({"different", "Different from your current password"});
    rules.push_back({"match", "Both passwords match"});

    string items;
    for (auto& r : rules) {
        bool bad = true, passed = false;
        if (failed != nullptr) {
            bad = false;
            for (auto& f : *failed)
                if (f == r.key) bad = true;
            passed = !bad;
        }
        items += render("pw_rule_item", {{"key", r.key}, {"cls", passed ? "pw-passed" : "bad"}, {"mark", passed ? "\u2713" : "\u2717"}, {"label", r.label}});
    }
    return render("pw_rules", {{"min", to_string(PW_MIN_LEN)}, {"max", to_string(PW_MAX_LEN)}, {"common", common},
                               {"state", failed != nullptr ? " server" : ""}, {"items", items}});
}

// signup page aur signup popup ke password hisse (password, confirm, checklist). change_form = /account/password.
// failed = server ne jo niyam fail paye (key); nullptr = galti nahi (list chhupi rehti hai)
static map<string, string> pw_parts(bool change_form, const vector<string>* failed) {
    return {{"password_field", pw_field_html(change_form ? "new_password" : "password", "new", "new-password", change_form ? "" : "Password", PW_MAX_LEN)},
            {"confirm_field", pw_field_html("confirm_password", "confirm", "new-password", change_form ? "" : "Confirm password", PW_MAX_LEN)},
            {"rules", pw_rules_html(change_form, failed)}};
}

// galtiyon ka laal box. Sirf ek simple galti ho to seedha text (jaise pehle), kai ho to list
static string errors_flash(const vector<string>& errs, bool as_list) {
    if (!as_list && errs.size() == 1) return flash_text("err", errs[0]);
    string items;
    for (auto& e : errs) items += render("error_list_item", {{"text", e}});
    return flash_box("err", render("error_list", {{"items", items}}));
}

static string signup_form(const string& name, const string& email, const vector<string>* failed = nullptr) {
    map<string, string> v = pw_parts(false, failed); // password fields kabhi dobara nahi bharte: sirf naam aur email wapas
    v["name"] = name;
    v["email"] = email;
    return render("signup", v);
}

static string login_form(const string& email) {
    return render("login", {{"email", email}});
}

// email simple check: ek '@', dono taraf kuch text, space nahi
static bool email_ok(const string& e) {
    size_t at = e.find('@');
    if (at == string::npos || at == 0 || at + 1 >= e.size()) return false;
    if (e.find('@', at + 1) != string::npos) return false;
    return e.find_first_of(" \t\r\n") == string::npos;
}

// signup form ke naam / email theek hain? galat ho to message return karta hai, theek ho to "" (password ke niyam check_password me)
static string signup_error(const string& name, const string& email) {
    if (name.empty() || email.empty()) return "Please fill in all fields.";
    if (name.size() > 100) return "Name is too long (max 100 characters).";
    if (email.size() > 100) return "Email is too long (max 100 characters).";
    if (!email_ok(email)) return "Please enter a valid email address.";
    return "";
}

// ---- auth pages ----

static int signup_page(SOCKET client, const Request& req) {
    if (req.method == "GET") return send_page(client, req, 200, "Sign up", signup_form("", ""));
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    string name = trim(field(req, "name"));
    string email = trim(field(req, "email"));
    string pass = field(req, "password");
    string confirm = field(req, "confirm_password");

    // saari galtiyan ek saath (user ek-ek karke na theek kare): naam/email, dono password milte hain ya nahi, aur fail hue niyam
    vector<string> errs;
    vector<string> failed;                  // fail hue password niyamon ke key: form ki list me sirf wahi dikhte hain
    bool pw_problem = false;                // password se judi galti ho to hamesha list
    if (pass.empty() || confirm.empty()) {
        string e = signup_error(name, email);
        errs.push_back(e.empty() ? "Please fill in all fields." : e);
    } else {
        string e = signup_error(name, email);
        if (!e.empty()) errs.push_back(e);
        if (pass != confirm) { errs.push_back("The two passwords do not match."); failed.push_back("match"); pw_problem = true; }
        for (auto& r : pw_failures(pass, name, email, "")) { errs.push_back(r.second); failed.push_back(r.first); pw_problem = true; }
    }
    if (!errs.empty())
        return send_page(client, req, 400, "Sign up", signup_form(name, email, pw_problem ? &failed : nullptr), "", errors_flash(errs, pw_problem));

    // password abhi plain text me hai (project ki known limitation; baad me hash karna hai)
    int code = 0;
    int n = db_exec("INSERT INTO users (name, email, password) VALUES (?, ?, ?)", {name, email, pass}, &code);
    if (n < 0) {
        if (code == 1062)   // duplicate entry: email pehle se hai (UNIQUE)
            return send_page(client, req, 409, "Sign up", signup_form(name, email), "",
                             flash_text("err", "Email already registered."));
        return error_page(client, req, 500, "Database error");
    }
    if (n != 1) return error_page(client, req, 500, "Could not create account");   // ek row judni chahiye thi
    log_tag("PAGE", "new user signed up");
    return redirect(client, req, "/login?registered=1");
}

static int login_page(SOCKET client, const Request& req) {
    if (req.method == "GET") {
        string ok = (param(req, "registered") == "1") ? flash_text("ok", "Account created. Please log in.") : "";
        return send_page(client, req, 200, "Login", login_form(""), "", ok);
    }
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    string email = trim(field(req, "email"));
    string pass = field(req, "password");
    const string bad = "Invalid email or password.";   // email galat ho ya password, message ek hi

    // BINARY: password ka match capital/small letters ke hisaab se (normal compare case-insensitive hota hai)
    vector<vector<string>> rows;
    if (!db_select("SELECT user_id FROM users WHERE email = ? AND BINARY password = ?", {email, pass}, rows))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) {
        log_tag("PAGE", "login failed");
        return send_page(client, req, 401, "Login", login_form(email), "", flash_text("err", bad));
    }
    if (!cols_ok(rows, 1)) return error_page(client, req, 500, "Database error");
    string uid = rows[0][0];             // login par password ke niyam kabhi nahi dekhte: purane passwords chalte rehte hain

    string sid;
    if (!new_session_id(sid)) return error_page(client, req, 500, "Could not create session");

    // expires_at = abhi + SESSION_MINUTES (aage har request par badhta rehta hai: idle timeout, load_user dekho)
    // INSERT fail hua (ya 1 row nahi judi) to cookie set NAHI karte: warna browser ke paas aisi session id hogi
    // jo DB me hai hi nahi
    int n = db_exec("INSERT INTO user_sessions (session_id, user_id, expires_at) VALUES (?, ?, " + EXPIRES_SQL + ")",
                    {sid, uid});
    if (n != 1) {
        log_tag("PAGE", "login: could not store session for user " + uid);
        return error_page(client, req, 500, "Could not log you in. Please try again.");
    }

    log_tag("PAGE", "user " + uid + " logged in");
    return redirect(client, req, "/products", session_cookie(sid, SESSION_MINUTES * 60));
}

// ---- /account/password ----

static string change_pw_form(const Request& req, const string& email, const vector<string>* failed = nullptr) {
    map<string, string> v = pw_parts(true, failed);
    v["current_field"] = pw_field_html("current_password", "old", "current-password", "", 255);   // purana password 64 se lamba bhi ho sakta hai
    v["name"] = req.user_name;
    v["email"] = email;
    return render("change_password", v);
}

// GET /account/password = form, POST = badlo. Login zaroori. Purana password sahi hona chahiye (db.cpp ka password_change dekhta hai)
static int account_password_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET" && req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    vector<vector<string>> rows;
    if (!db_select("SELECT email FROM users WHERE user_id = ?", {to_string(req.user_id)}, rows) || !cols_ok(rows, 1))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) return error_page(client, req, 404, "User not found");
    const string email = rows[0][0];

    if (req.method == "GET") {
        string flash;
        if (param(req, "ok") == "1") flash = flash_text("ok", "Password changed");
        return send_page(client, req, 200, "Change password", change_pw_form(req, email), "", flash);
    }

    string cur = field(req, "current_password");
    string pw = field(req, "new_password");
    string confirm = field(req, "confirm_password");
    vector<string> errs, failed;
    if (cur.empty()) errs.push_back("Please enter your current password.");
    if (pw.empty() || confirm.empty()) errs.push_back("Please enter the new password twice.");
    else {
        if (pw != confirm) { errs.push_back("The two new passwords do not match."); failed.push_back("match"); }
        for (auto& r : pw_failures(pw, req.user_name, email, cur)) { errs.push_back(r.second); failed.push_back(r.first); }
    }
    if (!errs.empty())
        return send_page(client, req, 400, "Change password", change_pw_form(req, email, failed.empty() ? nullptr : &failed), "", errors_flash(errs, true));

    PasswordResult r = password_change(req.user_id, cur, pw, req.session_id);
    if (r.status == 200) {
        log_tag("PAGE", "user " + to_string(req.user_id) + " changed the password, " + to_string(r.ended) + " other sessions ended");
        return redirect(client, req, "/account/password?ok=1");
    }
    if (r.status == 403) return send_page(client, req, 403, "Change password", change_pw_form(req, email), "", flash_text("err", r.message));
    return error_page(client, req, r.status == 503 ? 503 : 500, r.status == 503 ? r.message : "Database error");
}

// /logout sirf POST: session band, cookie hatao, /products par bhejo
static int logout_page(SOCKET client, const Request& req) {
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    if (req.user_id > 0) {
        // DB error ho to cookie mat hatao: session DB me active hai, user ko "logged out" dikhana galat hoga
        if (db_exec("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'LOGOUT' "
                    "WHERE session_id = ?",
                    {req.session_id}) < 0)
            return error_page(client, req, 500, "Could not log you out. Please try again.");
        log_tag("PAGE", "user " + to_string(req.user_id) + " logged out");
    }
    return redirect(client, req, "/products", session_cookie("", 0));
}

// cleanup thread ke liye: jo sessions expire ho chuke par abhi active hain, sab ek UPDATE me band karo
// logout_time = expires_at (asli expire ka time, cleanup ka time nahi). Kitne band hue wo return hota hai
int close_expired_sessions() {
    return db_exec("UPDATE user_sessions SET is_active = 0, logout_time = expires_at, end_reason = 'EXPIRED' "
                   "WHERE is_active = 1 AND expires_at < NOW()", {});
}

// status text: Active / Logged out / Expired
static string session_status(const string& is_active, const string& expired, const string& end_reason) {
    if (is_active == "1") return expired == "1" ? "Expired" : "Active";   // expire hua par abhi band nahi kiya gaya
    if (end_reason == "FORCED") return "Ended by admin";                  // admin ne band kiya (Admin > Sessions)
    return end_reason == "EXPIRED" ? "Expired" : "Logged out";            // LOGOUT ya purani row (NULL)
}

// /sessions: is user ki login history, current session highlight
static int sessions_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");

    vector<vector<string>> rows;
    if (!db_select("SELECT session_id, login_time, last_activity, logout_time, is_active, expires_at <= NOW(), "
                   "end_reason FROM user_sessions WHERE user_id = ? ORDER BY login_time DESC LIMIT 50",
                   {to_string(req.user_id)}, rows))
        return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 7)) return error_page(client, req, 500, "Database error");

    string body;
    for (auto& r : rows) {
        bool current = (r[0] == req.session_id);
        // poori session id kabhi page par nahi dikhate, sirf shuruaat ke 8 characters
        string status = session_status(r[4], r[5], r[6]);
        body += render("session_row", {{"row_class", current ? "me" : ""},
                                       {"session", r[0].substr(0, 8)},
                                       {"login", r[1]},
                                       {"activity", r[2]},
                                       {"logout", r[3].empty() ? "-" : r[3]},
                                       {"status", status},
                                       {"status_class", status_class(status)},
                                       {"current", current ? " (this session)" : ""}});
    }
    return send_page(client, req, 200, "My Sessions", render("sessions", {{"rows", body}}));
}

// ---- shop pages ----

// /test: jo request mili wahi dikhao
static int test_page(SOCKET client, const Request& req) {
    string params, headers;
    for (auto& kv : req.params)
        params += render("test_item", {{"key", kv.first}, {"sep", " = "}, {"value", kv.second}});
    for (auto& kv : req.headers) {
        // cookie me session id hota hai, use page par nahi dikhate
        string val = (kv.first == "cookie") ? "(hidden)" : kv.second;
        headers += render("test_item", {{"key", kv.first}, {"sep", ": "}, {"value", val}});
    }
    return send_page(client, req, 200, "Request",
                     render("test", {{"method", req.method}, {"path", req.path}, {"version", req.version},
                                     {"body_size", to_string(req.body.size())},
                                     {"params", params}, {"headers", headers}}));
}

// sql chalao pool wale connection se aur result, connection number, thread number dikhao (sirf testing)
static int db_test_page(SOCKET client, const Request& req, const string& title, const string& sql) {
    string val;
    int conn = 0;
    if (!db_scalar(sql, val, &conn)) return error_page(client, req, 500, "Database error");
    return send_page(client, req, 200, title,
                     render("dbtest", {{"title", title}, {"result", val}, {"conn", to_string(conn)},
                                       {"thread", to_string(thread_id())}}));
}

// ---- "Add to cart" ke baad usi page par wapas aana ----

// target se ek query param hata do: strip_param("/products?cat=3&added=5", "added") -> "/products?cat=3"
static string strip_param(const string& target, const string& name) {
    size_t q = target.find('?');
    if (q == string::npos) return target;

    string kept;
    size_t pos = q + 1;
    while (pos <= target.size()) {
        size_t amp = target.find('&', pos);
        if (amp == string::npos) amp = target.size();
        string pair = target.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty()) continue;
        if (pair.substr(0, pair.find('=')) == name) continue;   // yeh param hatana hai
        kept += (kept.empty() ? "" : "&") + pair;
    }
    return kept.empty() ? target.substr(0, q) : target.substr(0, q) + "?" + kept;
}

// is page ka path + query, purane "added"/"cart_err" ke bina: form ke hidden "return" field me jaata hai
static string return_target(const Request& req) {
    return strip_param(strip_param(req.target, "added"), "cart_err");
}

// OPEN REDIRECT se bachao: "return" form se aata hai, yani koi bhi likh sakta hai (return=//evil.com ya
// https://evil.com bana kar user ko dusri site par bhej sakta hai). Isliye sirf hamari site ke path chalenge:
//  - "/" se shuru ho, par "//" se nahi ("//evil.com" ka matlab dusri site hota hai)
//  - "\" nahi (kuch browser "\" ko "/" samajhte hain)
//  - space/control characters nahi (\r \n se Location header me nayi header line jodi ja sakti hai)
// kuch bhi galat ho to /products
static string safe_return(const string& r) {
    const string fallback = "/products";
    if (r.empty() || r.size() > 1000 || r[0] != '/') return fallback;
    if (r.size() > 1 && r[1] == '/') return fallback;
    for (char c : r)
        if (c == '\\' || c <= 32 || c == 127) return fallback;   // c <= 32 me non-ASCII bytes bhi aa jaate hain
    return r;
}

// ?added=<product id> (aur optional cart_err) ho to upar ka message: hara (add ho gaya) ya laal (problem)
//   cart_err=stock -> "Only N left", cart_err=out -> "out of stock". Text hamare fixed hain; product ka naam DB se
//   aur escape hota hai; added sirf number hone par use hota hai.
static string cart_notice(const Request& req) {
    string id = param(req, "added");
    if (!is_number(id) || atoi(id.c_str()) < 1) return "";

    vector<vector<string>> rows;
    if (!db_select("SELECT name, stock FROM products WHERE product_id = ?", {id}, rows)) return "";
    if (rows.empty() || !cols_ok(rows, 2)) return "";
    string name = rows[0][0];
    string err = param(req, "cart_err");
    int stock = atoi(rows[0][1].c_str());

    // message ke tukde templates me hain; naam engine escape karta hai
    if (err == "max")
        return flash_box("err", render("flash_max", {{"max", to_string(MAX_CART_QTY)}, {"name", name}}));
    if (err == "out" || (err == "stock" && stock == 0))
        return flash_box("err", render("flash_out", {{"name", name}}));
    if (err == "stock")
        return flash_box("err", render("flash_stock", {{"stock", to_string(stock)}, {"name", name}}));
    return flash_box("ok", render("flash_added", {{"name", name}}));
}

// product ki photo (public/images/products/<id>.jpg) ho to <img>, warna category emoji (admin ke naye products ke liye)
static string product_thumb(const string& id, const string& category) {
    string src = "images/products/" + id + ".jpg";
    if (find_static(src) != nullptr) return render("thumb_photo", {{"src", src}});
    return render("thumb_emoji", {{"emoji", category_emoji(category)}});
}

// ek product ka card (home, products, har jagah wahi): rangeen dibba + category emoji, naam, ek line ka description,
// ₹ price, stock badge aur poori chaudai ka button. back = is page ka path+query (add ke baad yahin wapas)
// Button: logged-in = "Add to cart" (POST), guest = "Sign in to buy" (login popup kholta hai), stock 0 = disabled "Out of stock"
// (server bhi /cart/add par mana karta hai: cart_add_steps). POST me sirf product_id aur return jaata hai: price/cart_id server nikalta hai
static string product_card(const vector<string>& p, const string& back, bool logged_in) {
    bool sold_out = (p[4] == "0");
    string action = sold_out ? render("soldout", {})
                  : logged_in ? render("add_form_card", {{"id", p[0]}, {"back", back}})
                              : render("signin_buy", {});
    return render("product_card", {{"hue", to_string(thumb_hue(p[2]))}, {"thumb", product_thumb(p[0], p[2])},
                                   {"id", p[0]}, {"name", p[1]}, {"description", p[5]}, {"price", rupees_text(p[3])},
                                   {"stock", stock_html(p[4])}, {"action", action}});
}

// /products?cat=ID&q=text&min=N&max=N  (navbar search, sidebar ki category aur price filter, "Under ₹1,000" link)
static int products_page(SOCKET client, const Request& req) {
    string cat = param(req, "cat");
    string q = param(req, "q");
    string min_price = param(req, "min");
    string max_price = param(req, "max");
    if (!cat.empty() && !is_number(cat)) return error_page(client, req, 400, "Bad category id");
    if ((!min_price.empty() && !is_number(min_price)) || (!max_price.empty() && !is_number(max_price)))
        return error_page(client, req, 400, "Bad price filter");

    vector<vector<string>> cats = get_categories(true);

    // SQL ke tukde fixed hain; user ka data sirf args me jaata hai (? ki jagah): category, naam, min aur max price
    string sql = "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description "
                 "FROM products p JOIN categories c ON c.category_id = p.category_id WHERE p.is_active = TRUE";
    vector<string> args;
    if (!cat.empty()) {
        sql += " AND p.category_id = ?";
        args.push_back(cat);
    }
    if (!q.empty()) {
        sql += " AND p.name LIKE ? ESCAPE '!'";
        args.push_back("%" + escape_like(q) + "%");
    }
    if (!min_price.empty()) {
        sql += " AND p.price >= ?";
        args.push_back(min_price);
    }
    if (!max_price.empty()) {
        sql += " AND p.price <= ?";
        args.push_back(max_price);
    }
    sql += " ORDER BY p.name LIMIT 200";

    vector<vector<string>> prods;
    if (!db_select(sql, args, prods)) return error_page(client, req, 500, "Database error");
    if (!cols_ok(prods, 6)) return error_page(client, req, 500, "Database error");

    string cards;
    string back = return_target(req);
    for (auto& p : prods) cards += product_card(p, back, req.user_id > 0);

    // sidebar: All + har category (chuni hui "on"), aur price form jo cat / q ko hidden fields me sambhalta hai
    string side = render("side_cat", {{"on_class", cat.empty() ? " on" : ""}, {"href", "/products"},
                                      {"emoji", "\xF0\x9F\x9B\x8D\xEF\xB8\x8F"}, {"label", "All"}});
    string heading = "All products";
    for (auto& c : cats) {
        bool on = (c[0] == cat);
        side += render("side_cat", {{"on_class", on ? " on" : ""}, {"href", "/products?cat=" + c[0]},
                                    {"emoji", category_emoji(c[1])}, {"label", c[1]}});
        if (on) heading = category_emoji(c[1]) + " " + c[1];
    }
    string hidden;
    if (!cat.empty()) hidden += render("hidden_input", {{"name", "cat"}, {"value", cat}});
    if (!q.empty()) hidden += render("hidden_input", {{"name", "q"}, {"value", q}});

    string count = to_string(prods.size()) + (prods.size() == 1 ? " product" : " products");
    string note;
    if (!q.empty()) note += " matching \"" + q + "\"";
    if (!min_price.empty()) note += ", from " + rupees(atoll(min_price.c_str()) * 100);
    if (!max_price.empty()) note += ", up to " + rupees(atoll(max_price.c_str()) * 100);

    string content = render("products", {{"side_cats", side}, {"hidden_fields", hidden}, {"min", min_price},
                                         {"max", max_price}, {"heading", heading}, {"count", count}, {"note", note},
                                         {"empty_message", prods.empty() ? render("no_products", {}) : ""},
                                         {"cards", cards}});
    return send_page(client, req, 200, "Products", content, "", cart_notice(req));
}

// Home page: hero, 4 tiles, "Shop by category" (emoji tiles -> /products?cat=N) aur "Budget picks under ₹1,000" (wahi card).
// Database na chale to bhi hero aur tiles dikhte hain (category / budget hisse bas nahi aate): home page kabhi 500 nahi deta
static int home_page(SOCKET client, const Request& req) {
    vector<vector<string>> cats = get_categories(true);
    vector<vector<string>> budget;
    if (!db_select(BUDGET_SQL, {}, budget) || !cols_ok(budget, 6)) {
        log_tag("PAGE", "home: could not load the budget picks");
        budget.clear();
    }

    string tiles;
    for (auto& c : cats)
        tiles += render("category_tile", {{"id", c[0]}, {"emoji", category_emoji(c[1])}, {"name", c[1]}});

    string cards;
    string back = return_target(req);       // "Add to cart" ke baad yahin (home par) wapas aate hain
    for (auto& p : budget) cards += product_card(p, back, req.user_id > 0);

    string budget_text = rupees((long long)BUDGET_MAX * 100);
    string content = render("home", {
        {"hero_second", req.user_id > 0 ? render("hero_orders", {}) : render("hero_create", {})},
        {"categories_section", cats.empty() ? "" : render("home_categories", {{"tiles", tiles}})},
        {"budget_section", budget.empty() ? "" : render("home_budget", {{"cards", cards}, {"budget", budget_text},
                                                                         {"budget_max", to_string(BUDGET_MAX)}})}});
    return send_page(client, req, 200, "Home", content, "", cart_notice(req));
}

// /product?id=N
static int product_page(SOCKET client, const Request& req) {
    string id = param(req, "id");
    if (!is_number(id)) return error_page(client, req, 400, "Bad product id");

    vector<vector<string>> rows;
    if (!db_select("SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.description, p.category_id "
                   "FROM products p JOIN categories c ON c.category_id = p.category_id "
                   "WHERE p.product_id = ? AND p.is_active = TRUE", {id}, rows))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) return error_page(client, req, 404, "Product not found");
    if (!cols_ok(rows, 7)) return error_page(client, req, 500, "Database error");

    vector<string>& p = rows[0];
    bool sold_out = (p[4] == "0");
    string action = sold_out ? render("soldout", {})
                  : req.user_id > 0 ? render("add_form_page", {{"id", p[0]}, {"back", return_target(req)},
                                                                {"max", to_string(MAX_CART_QTY)}})
                                    : render("signin_buy", {});
    string content = render("product", {{"hue", to_string(thumb_hue(p[2]))}, {"thumb", product_thumb(p[0], p[2])},
                                        {"name", p[1]}, {"category", p[2]}, {"cat_id", p[6]}, {"description", p[5]},
                                        {"price", rupees_text(p[3])}, {"stock", stock_html(p[4])}, {"action", action}});
    return send_page(client, req, 200, p[1], content, "", cart_notice(req));
}

// ---- cart ----
// Har cart page login maangta hai. cart_id hamesha logged-in user (session) se aata hai, form se kabhi nahi,
// aur price hamesha DB se. Form se sirf product_id aur qty liye jaate hain.

// text ek number hai aur lo..hi ke beech? to out me daalo
static bool parse_range(const string& s, int lo, int hi, int& out) {
    if (!is_number(s)) return false;
    int v = atoi(s.c_str());
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

// product_id valid hai? (number, 1 ya zyada)
static bool valid_product_id(const string& s) {
    int v = 0;
    return parse_range(s, 1, 999999999, v);
}

// cart ke items padho; cart na ho to rows khali. Ek hi connection, aur put_conn hamesha hota hai (return ek hi jagah)
static bool load_cart_rows(int user_id, vector<vector<string>>& rows) {
    rows.clear();
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return false;
    bool ok = true;
    int cart_id = find_active_cart(m, user_id);
    if (cart_id > 0) ok = cart_items(m, cart_id, rows);
    put_conn(id);
    return ok;
}

// ek item ki line: naam, price, qty (update form), subtotal, remove button, stock warning
static string cart_row(const vector<string>& r, long long& total) {
    int qty = atoi(r[3].c_str());
    int stock = atoi(r[4].c_str());
    long long sub = to_cents(r[2]) * qty;
    total += sub;

    string warn;
    if (r.size() > 5 && r[5] == "0") warn = render("stock_warn", {{"message", "No longer available"}});   // admin ne hide kiya
    else if (stock == 0) warn = render("stock_warn", {{"message", "Out of stock"}});
    else if (qty > stock) warn = render("stock_warn", {{"message", "Only " + to_string(stock) + " left"}});

    return render("cart_row", {{"id", r[0]}, {"name", r[1]}, {"warn", warn}, {"price", rupees_text(r[2])},
                               {"qty", to_string(qty)}, {"max", to_string(MAX_CART_QTY)}, {"subtotal", rupees(sub)}});
}

// GET /cart
static int cart_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    vector<vector<string>> rows;
    if (!load_cart_rows(req.user_id, rows)) return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 5)) return error_page(client, req, 500, "Database error");

    if (rows.empty())
        return send_page(client, req, 200, "Cart", render("heading", {{"text", "Your cart"}}) +
                                                    render("empty_state", {{"message", "Your cart is empty."}}));

    long long total = 0;
    string body;
    for (auto& r : rows) body += cart_row(r, total);
    return send_page(client, req, 200, "Cart", render("cart", {{"rows", body}, {"total", rupees(total)}}));
}

// cart/add ke andar ka kaam (m diya hua hai; get/put bahar hota hai). Status return karta hai, galti ho to msg me.
// problem (200 ke saath): "" = sab theek, "out" = stock 0 tha (add nahi hua), "stock" = add hua par cart me
// quantity ab stock se zyada hai. Page par isse laal message dikhta hai
static int cart_add_steps(MYSQL* m, int user_id, const string& pid, const string& qty, string& msg,
                          string& problem) {
    problem = "";
    int cart_id = get_active_cart(m, user_id);
    if (cart_id == 0) { msg = "Database error"; return 500; }

    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT stock FROM products WHERE product_id = ? AND is_active = TRUE", {pid}, rows) || !cols_ok(rows, 1)) {
        msg = "Database error";
        return 500;
    }
    if (rows.empty()) { msg = "Product not found"; return 404; }
    int stock = atoi(rows[0][0].c_str());
    if (stock == 0) { problem = "out"; return 200; }

    // add se pehle cart me kitni quantity thi? (0 = item nahi tha). Isse pata chalta hai ki MAX_CART_QTY ne kuch kaata ya nahi.
    // (sirf message ke liye; asli add neeche ek atomic query hai, isliye yahan race se nuksan nahi)
    int before = 0;
    if (db_select_on(m, "SELECT quantity FROM cart_items WHERE cart_id = ? AND product_id = ?",
                     {to_string(cart_id), pid}, rows) && !rows.empty() && !rows[0].empty())
        before = atoi(rows[0][0].c_str());

    if (!cart_add(m, cart_id, pid, qty)) { msg = "Database error"; return 500; }

    // MAX_CART_QTY se upar jaata to cap laga: "added" nahi, "Maximum N per item" dikhana hai
    if (before + atoi(qty.c_str()) > MAX_CART_QTY) { problem = "max"; return 200; }

    // stock se zyada cart me ho sakta hai (stock baad me bhi badal sakta hai): bas user ko bata dete hain
    // (nayi quantity ek select se: add ke baad ka sach wahi hai, jo atomic query ne banaya)
    if (db_select_on(m, "SELECT quantity FROM cart_items WHERE cart_id = ? AND product_id = ?",
                     {to_string(cart_id), pid}, rows) &&
        !rows.empty() && !rows[0].empty() && atoi(rows[0][0].c_str()) > stock)
        problem = "stock";
    return 200;
}

// POST /cart/add (product_id, qty: default 1, 1..MAX_CART_QTY)
static int cart_add_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    string pid = field(req, "product_id");
    string qty = field(req, "qty");
    if (qty.empty()) qty = "1";
    int q = 0;
    if (!valid_product_id(pid)) return error_page(client, req, 400, "Invalid product id");
    if (!parse_range(qty, 1, MAX_CART_QTY, q))
        return error_page(client, req, 400, "Quantity must be a number from 1 to " + to_string(MAX_CART_QTY));
    qty = to_string(q);                 // "007" jaisa text saaf number ban jaye

    string msg = "Database error";
    string problem;
    int status = 500;
    int id = 0;
    MYSQL* m = get_conn(id);            // is request ki saari queries isi ek connection par
    if (m != nullptr) {
        status = cart_add_steps(m, req.user_id, pid, qty, msg, problem);
        put_conn(id);                   // steps me kahin se bhi return ho, connection yahin wapas aata hai
    }
    if (status != 200) return error_page(client, req, status, msg);

    // Post/Redirect/Get: us page par wapas jahan se button dabaya tha, ?added=<id> ke saath.
    // "return" safe_return se guzarta hai (open redirect nahi); purane added/cart_err hata kar naye jodte hain
    string back = strip_param(strip_param(safe_return(field(req, "return")), "added"), "cart_err");
    back += (back.find('?') == string::npos ? "?" : "&") + string("added=") + pid;
    if (!problem.empty()) back += "&cart_err=" + problem;
    return redirect(client, req, back);
}

static int cart_update_steps(MYSQL* m, int user_id, const string& pid, const string& qty, string& msg) {
    int cart_id = get_active_cart(m, user_id);
    if (cart_id == 0) { msg = "Database error"; return 500; }
    int n = cart_set_qty(m, cart_id, pid, qty);     // qty "0" = item hat jata hai
    if (n < 0) { msg = "Database error"; return 500; }
    if (n == 0) { msg = "That item is not in your cart"; return 404; }
    return 200;
}

// POST /cart/update (product_id, qty: 0..MAX_CART_QTY, 0 = hata do)
static int cart_update_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    string pid = field(req, "product_id");
    string qty = field(req, "qty");
    int q = 0;
    if (!valid_product_id(pid)) return error_page(client, req, 400, "Invalid product id");
    if (!parse_range(qty, 0, MAX_CART_QTY, q))
        return error_page(client, req, 400, "Quantity must be a number from 0 to " + to_string(MAX_CART_QTY));
    qty = to_string(q);

    string msg = "Database error";
    int status = 500;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m != nullptr) {
        status = cart_update_steps(m, req.user_id, pid, qty, msg);
        put_conn(id);
    }
    if (status != 200) return error_page(client, req, status, msg);
    return redirect(client, req, "/cart");
}

static int cart_remove_steps(MYSQL* m, int user_id, const string& pid, string& msg) {
    int cart_id = find_active_cart(m, user_id);
    if (cart_id == 0) return 200;                   // cart hi nahi hai: hatane ko kuch nahi
    if (cart_remove(m, cart_id, pid) < 0) { msg = "Database error"; return 500; }
    return 200;                                     // item pehle se nahi tha (double click) to bhi theek
}

// POST /cart/remove (product_id)
static int cart_remove_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");

    string pid = field(req, "product_id");
    if (!valid_product_id(pid)) return error_page(client, req, 400, "Invalid product id");

    string msg = "Database error";
    int status = 500;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m != nullptr) {
        status = cart_remove_steps(m, req.user_id, pid, msg);
        put_conn(id);
    }
    if (status != 200) return error_page(client, req, status, msg);
    return redirect(client, req, "/cart");
}

// ---- shipping addresses ----

// address text ki 10 values r[o..o+9]: first, last, phone, line1, line2, landmark, city, state_name, pincode, type.
// Customer card, checkout, order page aur admin order page sab yahi use karte hain (order page par order_addresses ki copy se)
static string address_html(const vector<string>& r, size_t o) {
    string extra;                       // line2 aur landmark: khali ho to line hi nahi
    if (!r[o + 4].empty()) extra += render("address_extra", {{"text", r[o + 4]}});
    if (!r[o + 5].empty()) extra += render("address_extra", {{"text", "Landmark: " + r[o + 5]}});
    string type = r[o + 9];             // "HOME" -> "Home"
    for (size_t i = 0; i < type.size(); i++) type[i] = (i == 0) ? (char)toupper((unsigned char)type[i]) : (char)tolower((unsigned char)type[i]);
    return render("address_text", {{"name", r[o] + " " + r[o + 1]}, {"type", type}, {"type_class", status_class(r[o + 9])},
                                   {"line1", r[o + 3]}, {"extra", extra}, {"city", r[o + 6]}, {"state", r[o + 7]},
                                   {"pincode", r[o + 8]}, {"phone", r[o + 2]}});
}

// user ke active addresses (default pehle). Columns: address_id, 10 text values (address_html ke liye), is_default
static bool load_addresses(int user_id, vector<vector<string>>& rows) {
    if (!db_select("SELECT a.address_id, a.first_name, a.last_name, a.phone, a.line1, IFNULL(a.line2, ''), IFNULL(a.landmark, ''), "
                   "a.city, s.state_name, a.pincode, a.address_type, a.is_default FROM addresses a "
                   "JOIN states s ON s.state_id = a.state_id WHERE a.user_id = ? AND a.is_active = TRUE "
                   "ORDER BY a.is_default DESC, a.address_id", {to_string(user_id)}, rows))
        return false;
    return cols_ok(rows, 12);
}

// kisi order ka shipping address (order_addresses ki copy), order page / admin order page ke liye. Purana order = "No address"
static bool order_address_text(const string& order_id, string& out) {
    vector<vector<string>> rows;
    if (!db_select("SELECT first_name, last_name, phone, line1, IFNULL(line2, ''), IFNULL(landmark, ''), city, state_name, pincode, "
                   "address_type FROM order_addresses WHERE order_id = ?", {order_id}, rows) || !cols_ok(rows, 10))
        return false;
    out = rows.empty() ? render("address_none", {}) : address_html(rows[0], 0);
    return true;
}

// /addresses?ok=... ke upar ka message (text hamara fixed)
static string address_notice(const Request& req) {
    string ok = param(req, "ok");
    if (ok == "added") return flash_text("ok", "Address added.");
    if (ok == "saved") return flash_text("ok", "Address saved.");
    if (ok == "deleted") return flash_text("ok", "Address deleted. Orders you already placed keep their address.");
    if (ok == "default") return flash_text("ok", "Default address changed.");
    return "";
}

// GET /addresses: saare addresses cards me (Default badge, Edit, Delete, Set as default)
static int addresses_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    vector<vector<string>> rows;
    if (!load_addresses(req.user_id, rows)) return error_page(client, req, 500, "Database error");
    string content;
    if (rows.empty()) {
        content = render("heading", {{"text", "Your addresses"}}) + render("addresses_empty", {});
    } else {
        string cards;
        for (auto& r : rows) {
            bool def = (r[11] == "1");
            cards += render("address_card", {{"id", r[0]}, {"extra_class", def ? "addr-default" : ""},
                                             {"badge", def ? render("address_default_badge", {}) : ""},
                                             {"text", address_html(r, 1)},
                                             {"default_form", def ? "" : render("address_default_form", {{"id", r[0]}})}});
        }
        content = render("addresses", {{"count", to_string(rows.size())}, {"max", to_string(MAX_ADDRESSES)}, {"cards", cards}});
    }
    return send_page(client, req, 200, "Addresses", content, "", address_notice(req));
}

// form ke fields request se (galat ya khali ho to address_check message dega)
static Address read_address(const Request& req) {
    Address a;
    a.id = trim(field(req, "address_id"));
    a.first_name = field(req, "first_name");
    a.last_name = field(req, "last_name");
    a.phone = field(req, "phone");
    a.line1 = field(req, "line1");
    a.line2 = field(req, "line2");
    a.landmark = field(req, "landmark");
    a.city = field(req, "city");
    a.state_id = field(req, "state_id");
    a.pincode = field(req, "pincode");
    a.type = field(req, "address_type");
    return a;
}

// "checkout se aaye the" yaad rakho: add/edit ke baad wapas /checkout. Sirf yeh ek fixed value, koi URL nahi
static bool from_checkout(const Request& req) {
    return param(req, "return") == "checkout" || field(req, "return") == "checkout";
}

// address form: states ki list DB se (36 rows), type ke teen options, jo user ne bhara wo wapas
static int address_form_page(SOCKET client, const Request& req, int status, const Address& a, bool is_new, const string& error) {
    vector<vector<string>> states;
    if (!db_select("SELECT state_id, state_name FROM states ORDER BY state_name", {}, states) || !cols_ok(states, 2))
        return error_page(client, req, 500, "Database error");
    string state_opts;
    for (auto& st : states)
        state_opts += render("search_option", {{"value", st[0]}, {"label", st[1]}, {"selected", st[0] == a.state_id ? "selected" : ""}});
    string type_opts;
    for (const char* t : {"HOME", "WORK", "OTHER"}) {
        string label = string(1, t[0]) + (string(t + 1)) ;
        for (size_t i = 1; i < label.size(); i++) label[i] = (char)tolower((unsigned char)label[i]);
        type_opts += render("search_option", {{"value", t}, {"label", label}, {"selected", a.type == t ? "selected" : ""}});
    }
    string hidden;
    if (!is_new) hidden += render("hidden_input", {{"name", "address_id"}, {"value", a.id}});
    if (from_checkout(req)) hidden += render("hidden_input", {{"name", "return"}, {"value", "checkout"}});

    string content = render("address_form", {{"title", is_new ? "Add a new address" : "Edit address"},
                                             {"action", is_new ? "/addresses/new" : "/addresses/edit"}, {"hidden", hidden},
                                             {"first_name", a.first_name}, {"last_name", a.last_name}, {"phone", a.phone},
                                             {"types", type_opts}, {"line1", a.line1}, {"line2", a.line2}, {"landmark", a.landmark},
                                             {"city", a.city}, {"states", state_opts}, {"pincode", a.pincode},
                                             {"button", is_new ? "Save address" : "Save changes"},
                                             {"cancel", from_checkout(req) ? "/checkout" : "/addresses"}});
    return send_page(client, req, status, is_new ? "Add address" : "Edit address", content, "", error.empty() ? "" : flash_text("err", error));
}

// save ka nateeja: kamyab to wapas (checkout ya /addresses), galat input par form wapas message ke saath, baaki error page
static int address_saved(SOCKET client, const Request& req, const AddressResult& r, const Address& a, bool is_new) {
    if (r.status == 200) {
        if (from_checkout(req)) return redirect(client, req, "/checkout?address_id=" + to_string(r.address_id));
        return redirect(client, req, is_new ? "/addresses?ok=added" : "/addresses?ok=saved");
    }
    if (r.status == 400) return address_form_page(client, req, 400, a, is_new, r.message);
    return error_page(client, req, r.status == 404 || r.status == 503 ? r.status : 500, r.message);
}

// GET /addresses/new = khali form, POST = save. Sab checks server par (form ke pattern sirf suvidha hain)
static int address_new_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method == "GET") return address_form_page(client, req, 200, Address(), true, "");
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    Address a = read_address(req);
    a.id = "";
    string e = address_check(a);
    if (!e.empty()) return address_form_page(client, req, 400, a, true, e);
    return address_saved(client, req, address_save(req.user_id, a, true), a, true);
}

// GET /addresses/edit?id=N = bhara hua form, POST = save. Address sirf isi user ka (nahi to 404)
static int address_edit_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET" && req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    if (req.method == "POST") {
        Address a = read_address(req);
        if (!valid_product_id(a.id)) return error_page(client, req, 400, "Invalid address id");
        string e = address_check(a);
        if (!e.empty()) return address_form_page(client, req, 400, a, false, e);
        return address_saved(client, req, address_save(req.user_id, a, false), a, false);
    }

    string id = param(req, "id");
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    vector<vector<string>> rows;
    if (!db_select("SELECT first_name, last_name, phone, line1, IFNULL(line2, ''), IFNULL(landmark, ''), city, state_id, pincode, address_type "
                   "FROM addresses WHERE address_id = ? AND user_id = ? AND is_active = TRUE", {id, to_string(req.user_id)}, rows) ||
        !cols_ok(rows, 10))
        return error_page(client, req, 500, "Database error");
    if (rows.empty()) return error_page(client, req, 404, "Address not found");
    Address a;
    a.id = id;
    a.first_name = rows[0][0]; a.last_name = rows[0][1]; a.phone = rows[0][2]; a.line1 = rows[0][3]; a.line2 = rows[0][4];
    a.landmark = rows[0][5]; a.city = rows[0][6]; a.state_id = rows[0][7]; a.pincode = rows[0][8]; a.type = rows[0][9];
    return address_form_page(client, req, 200, a, false, "");
}

// POST /addresses/delete (address_id): is_active = 0, DELETE nahi
static int address_delete_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "address_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    AddressResult r = address_delete(req.user_id, atoi(id.c_str()));
    if (r.status == 200) return redirect(client, req, "/addresses?ok=deleted");
    return error_page(client, req, r.status == 404 ? 404 : 500, r.message);
}

// POST /addresses/default (address_id)
static int address_default_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "address_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid address id");
    AddressResult r = address_set_default(req.user_id, atoi(id.c_str()));
    if (r.status == 200) return redirect(client, req, "/addresses?ok=default");
    return error_page(client, req, r.status == 404 || r.status == 503 ? r.status : 500, r.message);
}

// ---- checkout aur orders ----

// ---- payment (simulated gateway: asli paise nahi katte, par jaanch asli jaisi) ----
// Net Banking ke bank: code form me jaata hai, naam payments.pay_detail me
struct Bank {
    const char* code;
    const char* name;
};
static const Bank BANKS[] = {{"SBI", "State Bank of India"}, {"HDFC", "HDFC Bank"}, {"ICICI", "ICICI Bank"}, {"AXIS", "Axis Bank"},
                             {"KOTAK", "Kotak Mahindra Bank"}, {"PNB", "Punjab National Bank"}, {"BOB", "Bank of Baroda"}};

// Test values (asli gateways ke test mode jaise): yeh card / UPI ID "bank ne mana kiya" dete hain, ROLLBACK dikhane ke liye
const string DECLINED_CARD = "4000000000000002";
const string DECLINED_UPI = "fail@upi";

// card number ka Luhn checksum (har card number me hota hai): galat type kiya ek bhi digit pakad leta hai
static bool luhn_ok(const string& digits) {
    int sum = 0;
    bool twice = false;                     // daayein se har doosra digit double
    for (size_t i = digits.size(); i-- > 0;) {
        int d = digits[i] - '0';
        if (twice) {
            d *= 2;
            if (d > 9) d -= 9;
        }
        sum += d;
        twice = !twice;
    }
    return sum % 10 == 0;
}

// number ki shuruaat se card ka brand (sirf dikhane ke liye)
static string card_brand(const string& d) {
    int two = atoi(d.substr(0, 2).c_str());
    int four = atoi(d.substr(0, 4).c_str());
    if (d[0] == '4') return "VISA";
    if ((two >= 51 && two <= 55) || (four >= 2221 && four <= 2720)) return "MASTERCARD";
    if (two == 34 || two == 37) return "AMEX";
    if (two == 60 || two == 65 || two == 81 || two == 82) return "RUPAY";
    return "CARD";
}

// expiry "MM/YY": sahi month aur is mahine ya baad ka (is mahine ke aakhir tak card chalta hai)
static bool expiry_ok(const string& e) {
    if (e.size() != 5 || e[2] != '/' || !is_number(e.substr(0, 2)) || !is_number(e.substr(3, 2))) return false;
    int mm = atoi(e.substr(0, 2).c_str());
    int yy = 2000 + atoi(e.substr(3, 2).c_str());
    if (mm < 1 || mm > 12) return false;
    time_t now = time(nullptr);
    tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    int cur = (t.tm_year + 1900) * 12 + t.tm_mon + 1;
    return yy * 12 + mm >= cur;
}

// UPI ID: naam@bank, jaise ravi.kumar@okaxis (naam: a-z 0-9 . _ -, bank: sirf letters)
static bool upi_ok(const string& u) {
    size_t at = u.find('@');
    if (at == string::npos || at < 2 || at > 64 || u.size() - at - 1 < 2 || u.size() - at - 1 > 32) return false;
    for (size_t i = 0; i < u.size(); i++) {
        char c = u[i];
        if (i < at && !(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
        if (i > at && !isalpha((unsigned char)c)) return false;
    }
    return true;
}

// gateway ka transaction id: "TXN" + 12 random hex (session id wale OS ke secure random se)
static string new_txn_ref() {
    string hex;
    if (!new_session_id(hex)) return "";
    string out = "TXN";
    for (size_t i = 0; i < 12; i++) out += (char)toupper((unsigned char)hex[i]);
    return out;
}

// checkout form ka payment padho aur jaancho. "" = theek (pay bhar gaya), warna user ko dikhane layak galti.
// Card number aur CVV sirf yahin jaanche jaate hain: na DB me jaate hain, na log me
static string read_payment(const Request& req, PaymentInfo& pay) {
    pay.method = trim(field(req, "pay_method"));
    if (pay.method == "COD") return "";

    if (pay.method == "UPI") {
        string upi = lower_ascii(trim(field(req, "upi_id")));
        if (!upi_ok(upi)) return "Enter a valid UPI ID, like name@okaxis.";
        pay.detail = upi;
        pay.declined = (upi == DECLINED_UPI);
    } else if (pay.method == "CARD") {
        string num;
        for (char c : field(req, "card_number")) {
            if (c == ' ' || c == '-') continue;                 // "4111 1111 ..." bhi chalta hai
            if (c < '0' || c > '9') return "Card number can only have digits.";
            num += c;
        }
        if (num.size() < 13 || num.size() > 19 || !luhn_ok(num)) return "That card number is not valid. Please check it.";
        string brand = card_brand(num);
        string name = trim(field(req, "card_name"));
        if (name.size() < 2 || name.size() > 60) return "Enter the name printed on the card.";
        if (!expiry_ok(trim(field(req, "card_expiry")))) return "Enter a valid expiry date (MM/YY) that is not in the past.";
        string cvv = trim(field(req, "card_cvv"));
        size_t want = (brand == "AMEX") ? 4 : 3;
        if (cvv.size() != want || !is_number(cvv)) return "CVV must be " + to_string(want) + " digits.";
        pay.detail = brand + " **** " + num.substr(num.size() - 4);
        pay.declined = (num == DECLINED_CARD);
    } else if (pay.method == "NETBANKING") {
        string code = trim(field(req, "bank"));
        for (const Bank& b : BANKS)
            if (code == b.code) pay.detail = b.name;
        if (pay.detail.empty()) return "Please choose your bank.";
    } else {
        return "Please choose a payment method.";
    }

    pay.txn_ref = new_txn_ref();
    if (pay.txn_ref.empty()) return "Could not start the payment. Please try again.";
    return "";
}

// checkout page ka payment panel. POST me galti ho to chuna hua tarika, UPI ID, naam, expiry, bank wapas bhare hote hain
// (card number aur CVV kabhi wapas nahi bhejte)
static string payment_panel(const Request& req) {
    bool post = req.method == "POST";
    string m = post ? trim(field(req, "pay_method")) : "";
    if (m != "UPI" && m != "CARD" && m != "NETBANKING") m = "COD";
    string bank = post ? field(req, "bank") : "";
    string banks = render("search_option", {{"value", ""}, {"label", "Choose your bank"}, {"selected", ""}});
    for (const Bank& b : BANKS)
        banks += render("search_option", {{"value", b.code}, {"label", b.name}, {"selected", bank == b.code ? "selected" : ""}});
    return render("checkout_payment", {{"cod", m == "COD" ? "checked" : ""}, {"upi", m == "UPI" ? "checked" : ""},
                                       {"card", m == "CARD" ? "checked" : ""}, {"netbanking", m == "NETBANKING" ? "checked" : ""},
                                       {"upi_id", post ? field(req, "upi_id") : ""}, {"card_name", post ? field(req, "card_name") : ""},
                                       {"card_expiry", post ? field(req, "card_expiry") : ""}, {"banks", banks}});
}

// payment ka dikhne wala hissa (order page aur admin order page): tarika + detail, status ki goli, kab paid, transaction id
static string payment_html(const string& method, const string& detail, const string& status, const string& paid, const string& txn) {
    if (method.empty()) return "-";
    string label = method == "COD" ? "Cash on Delivery" : method == "UPI" ? "UPI" : method == "CARD" ? "Card"
                 : method == "NETBANKING" ? "Net Banking" : method;
    return render("order_payment", {{"method", label}, {"detail", detail.empty() ? "" : " (" + detail + ")"},
                                    {"status", status}, {"status_class", status_class(status)},
                                    {"paid", paid.empty() ? "" : ", paid " + paid}, {"txn", txn.empty() ? "" : ", Txn " + txn}});
}

// checkout ka confirm page: cart ka saar aur total dikhao (sirf dekhna, kuch badalta nahi).
// status/error: payment form galat tha to wahi page 400 aur galti ke saath
static int checkout_confirm_page(SOCKET client, const Request& req, int status = 200, const string& error = "") {
    vector<vector<string>> rows;
    if (!load_cart_rows(req.user_id, rows)) return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 5)) return error_page(client, req, 500, "Database error");
    if (rows.empty())
        return send_page(client, req, 200, "Checkout", render("heading", {{"text", "Checkout"}}) +
                                                        render("empty_state", {{"message", "Your cart is empty."}}));

    long long total = 0;
    string body, warnings;
    for (auto& r : rows) {
        int qty = atoi(r[3].c_str());
        int stock = atoi(r[4].c_str());
        long long sub = to_cents(r[2]) * qty;
        total += sub;
        body += render("checkout_row", {{"name", r[1]}, {"qty", to_string(qty)}, {"price", rupees_text(r[2])},
                                        {"subtotal", rupees(sub)}});
        if (qty > stock)
            warnings += flash_text("err", r[1] + ": only " + to_string(stock) + " left, you have " + to_string(qty) +
                                          " in your cart. Update your cart first.");
    }

    // shipping address: radio cards. Default pehle aata hai (load_addresses ka order) aur wahi pehle se chuna hota hai.
    // /checkout?address_id=N (abhi add kiya hua address) ho to wo chuna hota hai
    vector<vector<string>> addrs;
    if (!load_addresses(req.user_id, addrs)) return error_page(client, req, 500, "Database error");
    string want = req.method == "POST" ? field(req, "address_id") : param(req, "address_id");
    size_t pick = 0;
    for (size_t i = 0; i < addrs.size(); i++)
        if (addrs[i][0] == want) pick = i;
    string cards;
    for (size_t i = 0; i < addrs.size(); i++)
        cards += render("checkout_address", {{"id", addrs[i][0]}, {"checked", i == pick ? "checked" : ""},
                                             {"badge", addrs[i][11] == "1" ? render("address_default_badge", {}) : ""},
                                             {"text", address_html(addrs[i], 1)}});
    if (addrs.empty()) cards = render("checkout_no_address", {});      // address ke bina order nahi: button hi nahi
    return send_page(client, req, status, "Checkout",
                     render("checkout", {{"warnings", warnings}, {"addresses", cards}, {"payment", payment_panel(req)},
                                         {"rows", body}, {"total", rupees(total)},
                                         {"place", addrs.empty() ? "" : render("checkout_place", {})}}),
                     "", error.empty() ? "" : flash_text("err", error));
}

// GET /checkout = confirm page, POST /checkout = order banao (ek DB transaction, db.cpp ka do_checkout)
static int checkout_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method == "GET") return checkout_confirm_page(client, req);
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    // cart session ke user ka. Form se sirf address_id: wo isi user ka aur active hai ya nahi, transaction ke andar db.cpp dekhta hai
    string aid = trim(field(req, "address_id"));
    if (!valid_product_id(aid))
        return send_page(client, req, 400, "Order not placed", render("checkout_failed", {}), "",
                         flash_text("err", "Please choose a shipping address."));
    // payment form galat ho to wahi checkout page galti ke saath (address, tarika, UPI ID wapas bhare), order ki koshish hi nahi
    PaymentInfo pay;
    string bad = read_payment(req, pay);
    if (!bad.empty()) return checkout_confirm_page(client, req, 400, bad);
    CheckoutResult r = do_checkout(req.user_id, atoi(aid.c_str()), pay);
    if (r.status == 200) return redirect(client, req, "/order?id=" + to_string(r.order_id));   // Post/Redirect/Get

    string retry = (r.status == 503) ? "Retry-After: 2\r\n" : "";
    return send_page(client, req, r.status, "Order not placed", render("checkout_failed", {}), retry,
                     flash_text("err", r.message));
}

// GET /orders: is user ke saare orders (naye pehle)
static int orders_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    vector<vector<string>> rows;
    if (!db_select("SELECT order_id, created_at, total, status FROM orders WHERE user_id = ? "
                   "ORDER BY order_id DESC LIMIT 50", {to_string(req.user_id)}, rows))
        return error_page(client, req, 500, "Database error");
    if (!cols_ok(rows, 4)) return error_page(client, req, 500, "Database error");

    if (rows.empty())
        return send_page(client, req, 200, "Orders", render("heading", {{"text", "Your orders"}}) +
                                                      render("empty_state", {{"message", "No orders yet."}}));
    string body;
    for (auto& r : rows)
        body += render("order_row", {{"id", r[0]}, {"placed", r[1]}, {"total", rupees_text(r[2])}, {"status", status_label(r[3])},
                                     {"status_class", status_class(r[3])}});
    return send_page(client, req, 200, "Orders", render("orders", {{"rows", body}}));
}

// order ka header (payment ke saath) aur items, ek hi connection par. Order sirf us user ka mile jisne manga
// ("AND o.user_id = ?"): doosre ka order mile hi nahi, isliye 404 (403 nahi: order hai ya nahi, yeh bhi nahi batate)
static bool load_order(MYSQL* m, int user_id, const string& id, vector<vector<string>>& head,
                       vector<vector<string>>& items, vector<vector<string>>& hist, string& eta) {
    if (!db_select_on(m, "SELECT o.order_id, o.status, o.total, o.created_at, IFNULL(p.method, ''), "
                         "IFNULL(p.status, ''), IFNULL(p.paid_at, ''), IFNULL(p.pay_detail, ''), IFNULL(p.txn_ref, '') "
                         "FROM orders o LEFT JOIN payments p ON p.order_id = o.order_id "
                         "WHERE o.order_id = ? AND o.user_id = ?", {id, to_string(user_id)}, head))
        return false;
    if (head.empty()) return true;
    if (!db_select_on(m, "SELECT p.product_id, p.name, oi.quantity, oi.price FROM order_items oi "
                         "JOIN products p ON p.product_id = oi.product_id WHERE oi.order_id = ? ORDER BY p.name",
                      {id}, items))
        return false;
    // tracking ke liye: har status kab bana (history) aur "Arriving by": agle step ka time + baaki steps ka average.
    // GREATEST(.., NOW()): agar agle step ka time nikal chuka (delivery thread abhi pahunchi nahi) to ab se ginte hain.
    // Seconds hamara apna int hai (order_eta_seconds), user ka text nahi
    if (!db_select_on(m, "SELECT status, DATE_FORMAT(changed_at, '%d %b, %H:%i:%s'), changed_by FROM order_status_history "
                         "WHERE order_id = ? ORDER BY history_id", {id}, hist))
        return false;
    const string status = head[0].size() > 1 ? head[0][1] : "";
    if (AUTO_DELIVERY && !order_next_status(status).empty()) {
        vector<vector<string>> e;
        if (!db_select_on(m, "SELECT DATE_FORMAT(DATE_ADD(GREATEST(next_step_at, NOW()), INTERVAL " + to_string(order_eta_seconds(status)) +
                             " SECOND), '%d %b, %H:%i:%s') FROM orders WHERE order_id = ? AND next_step_at IS NOT NULL", {id}, e))
            return false;
        if (!e.empty() && !e[0].empty()) eta = e[0][0];
    }
    return true;
}

// history me is status ka (pehla) time; nahi mila (purane order) to ""
static string history_time(const vector<vector<string>>& hist, const string& status) {
    for (auto& h : hist)
        if (h.size() >= 2 && h[0] == status) return h[1];
    return "";
}

// order tracking: 5 steps (Ordered, Packed, Shipped, Out for delivery, Delivered). Poore hue = tick + history ka time,
// abhi wala = highlight, baaki halke. CANCELLED order me jo steps hue wo tick, aakhir me laal "Cancelled".
static string timeline_html(const string& status, const vector<vector<string>>& hist, const string& eta) {
    static const char* STEPS[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
    bool cancelled = (status == "CANCELLED");
    int cur = -1;
    for (int i = 0; i < 5; i++)
        if (status == STEPS[i]) cur = i;

    string steps;
    for (int i = 0; i < 5; i++) {
        string t = history_time(hist, STEPS[i]);
        string state = "todo";
        if (cancelled) {
            if (t.empty()) continue;            // cancel hua to jo step hue hi nahi, wo dikhte nahi
            state = "done";
        }
        else if (status == "DELIVERED" || i < cur) state = "done";
        else if (i == cur) state = "current";
        steps += render("timeline_step", {{"state", state}, {"label", status_label(STEPS[i])}, {"time", state == "todo" ? "" : t}});
    }
    if (cancelled)
        steps += render("timeline_step", {{"state", "cancelled"}, {"label", "Cancelled"},
                                          {"time", history_time(hist, "CANCELLED")}});
    string eta_html = eta.empty() ? "" : render("timeline_eta", {{"time", eta}});
    return render("timeline", {{"steps", steps}, {"eta", eta_html}});
}

// GET /order?id=N
// order page ke upar ka message: /order?id=N&ok=cancelled (text hamara fixed)
static string order_notice(const Request& req) {
    if (param(req, "ok") == "cancelled") return flash_text("ok", "Your order was cancelled. Its items went back to stock. If you paid online, the money is refunded.");
    return "";
}

static int order_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    string id = param(req, "id");
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid order id");

    vector<vector<string>> head, items, hist;
    string eta;
    bool ok = false;
    int cid = 0;
    MYSQL* m = get_conn(cid);
    if (m != nullptr) {
        ok = load_order(m, req.user_id, id, head, items, hist, eta);
        put_conn(cid);
    }
    if (!ok) return error_page(client, req, 500, "Database error");
    if (head.empty()) return error_page(client, req, 404, "Order not found");
    if (!cols_ok(head, 9) || !cols_ok(items, 4) || !cols_ok(hist, 3)) return error_page(client, req, 500, "Database error");

    vector<string>& o = head[0];
    // payment: tarika + detail + status ki goli (PENDING peeli, PAID hari, REFUNDED ...); payment row na ho to "-"
    string pay = payment_html(o[4], o[7], o[5], o[6], o[8]);
    string body;
    for (auto& r : items)       // price = jo order ke waqt thi (order_items me), products ki aaj ki nahi
        body += render("order_item_row", {{"id", r[0]}, {"name", r[1]}, {"qty", r[2]}, {"price", rupees_text(r[3])},
                                          {"subtotal", rupees(to_cents(r[3]) * atoi(r[2].c_str()))}});
    string addr;                // shipping address (order ke waqt ki copy); purana order = "No address (old order)"
    if (!order_address_text(o[0], addr)) return error_page(client, req, 500, "Database error");
    // "Cancel order" sirf PLACED ya PACKED order par (server bhi yahi check karta hai: order_change)
    string actions = (o[1] == "PLACED" || o[1] == "PACKED") ? render("order_cancel", {{"id", o[0]}}) : "";
    return send_page(client, req, 200, "Order #" + o[0],
                     render("order", {{"id", o[0]}, {"status", status_label(o[1])}, {"status_class", status_class(o[1])},
                                      {"timeline", timeline_html(o[1], hist, eta)}, {"address", render("order_address", {{"body", addr}})},
                                      {"placed", o[3]}, {"payment", pay}, {"rows", body}, {"total", rupees_text(o[2])},
                                      {"actions", actions}}),
                     "", order_notice(req));
}

// POST /order/cancel (order_id): customer apna PLACED / PACKED order cancel kare. Owner check order_change ke andar (doosre ka order = 404)
static int order_cancel_page(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.method != "POST") return method_not_allowed(client, req, "POST");
    string id = trim(field(req, "order_id"));
    if (!valid_product_id(id)) return error_page(client, req, 400, "Invalid order id");

    OrderResult r = order_change(atoi(id.c_str()), "CANCELLED", req.user_id, false, "");
    if (r.status == 200) return redirect(client, req, "/order?id=" + id + "&ok=cancelled");
    if (r.status == 400) return error_page(client, req, 400, "This order can no longer be cancelled.");
    if (r.status == 404 || r.status == 409 || r.status == 503) return error_page(client, req, r.status, r.message);
    return error_page(client, req, 500, "Database error");
}

// ---- /metrics ----

// "3.14" (2 decimal)
static string num2(double v) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// 3725 -> "1h 2m 5s"
static string uptime_text(long long s) {
    return to_string(s / 3600) + "h " + to_string((s % 3600) / 60) + "m " + to_string(s % 60) + "s";
}

// metrics table ki ek row
static string metric_row(const string& name, const string& value) {
    return render("metric_row", {{"name", name}, {"value", value}});
}

// /metrics aur /metrics.json: andar ke numbers hain, isliye sirf isi computer (127.0.0.1) se, seedhe (proxy / tunnel ke through nahi).
// Server har network card par sunta hai (INADDR_ANY), isliye yeh check zaroori hai: dusre computer se 403
static int metrics_page(SOCKET client, const Request& req, bool json) {
    // X-Forwarded-For / X-Forwarded-Host = request kisi tunnel / proxy (jaise ngrok) se aayi: proxy local machine par chalta
    // hai, isliye socket ka IP 127.0.0.1 dikhta hai par asli client bahar ka hai. Aisi request ko bhi 403
    bool proxied = req.headers.count("x-forwarded-for") > 0 || req.headers.count("x-forwarded-host") > 0;
    if (req.client_ip != "127.0.0.1" || proxied)
        return send_page(client, req, 403, "Forbidden",
                         error_content("403 Forbidden", render("detail_text", {{"text", "Metrics are only available directly from 127.0.0.1 (not through a proxy or tunnel)."}})));
    if (req.method != "GET") return method_not_allowed(client, req, "GET");

    Metrics m = metrics_snapshot();

    if (json) {
        string j = "{\"uptime_seconds\":" + to_string(m.uptime_s) +
                   ",\"total_requests\":" + to_string(m.total) +
                   ",\"status\":{\"2xx\":" + to_string(m.c2xx) + ",\"3xx\":" + to_string(m.c3xx) +
                   ",\"4xx\":" + to_string(m.c4xx) + ",\"5xx\":" + to_string(m.c5xx) + "}" +
                   ",\"rejected_503\":" + to_string(m.rejected_503) +
                   ",\"timeouts_408\":" + to_string(m.timeouts_408) +
                   ",\"busy_workers\":" + to_string(m.busy_workers) + ",\"workers\":" + to_string(WORKERS) +
                   ",\"queue_length\":" + to_string(m.queue_length) + ",\"queue_size\":" + to_string(QUEUE_SIZE) +
                   ",\"free_db_connections\":" + to_string(m.free_db_conns) + ",\"db_pool\":" + to_string(DB_POOL) +
                   ",\"latency_ms\":{\"avg\":" + num2(m.avg_ms) + ",\"p95\":" + num2(m.p95_ms) +
                   ",\"samples\":" + to_string(m.latency_samples) + "}" +
                   ",\"requests_per_sec\":" + num2(m.rps) +
                   ",\"checkout\":{\"commit\":" + to_string(m.checkout_commit) +
                   ",\"rollback\":" + to_string(m.checkout_rollback) + "}" +
                   ",\"dropped_logs\":" + to_string(m.dropped_logs) + "}";
        send_response(client, 200, "application/json", j, "Cache-Control: no-store\r\n");
        return 200;
    }

    // simple table; auto refresh: <meta http-equiv="refresh"> (JavaScript nahi)
    string h;
    h += metric_row("Uptime", uptime_text(m.uptime_s));
    h += metric_row("Total requests", to_string(m.total));
    h += metric_row("2xx / 3xx / 4xx / 5xx", to_string(m.c2xx) + " / " + to_string(m.c3xx) + " / " + to_string(m.c4xx) + " / " +
                                       to_string(m.c5xx));
    h += metric_row("503 rejected (queue full)", to_string(m.rejected_503));
    h += metric_row("408 timeouts", to_string(m.timeouts_408));
    h += metric_row("Busy workers now", to_string(m.busy_workers) + " / " + to_string(WORKERS));
    h += metric_row("Queue length now", to_string(m.queue_length) + " / " + to_string(QUEUE_SIZE));
    h += metric_row("Free DB connections now", to_string(m.free_db_conns) + " / " + to_string(DB_POOL));
    h += metric_row("Latency avg (last " + to_string(m.latency_samples) + " requests)", num2(m.avg_ms) + " ms");
    h += metric_row("Latency p95 (last " + to_string(m.latency_samples) + " requests)", num2(m.p95_ms) + " ms");
    h += metric_row("Requests/sec (last " + to_string(RPS_SECONDS) + " s)", num2(m.rps));
    h += metric_row("Checkout COMMIT / ROLLBACK", to_string(m.checkout_commit) + " / " + to_string(m.checkout_rollback));
    h += metric_row("Dropped logs", to_string(m.dropped_logs));
    // interactive = false: is page par koi JavaScript nahi (auto refresh meta tag se hota hai)
    send_response(client, 200, HTML, page(req, "Metrics", render("metrics", {{"rows", h}}),
                                          render("meta_refresh", {{"seconds", "2"}}), "", false, false),
                  "Cache-Control: no-store\r\n");
    return 200;
}

// ---- admin panel (A1: dashboard, A2: products + categories) ----
// Har /admin/* route sabse pehle require_admin se guzarta hai (route() me ek jagah, taaki koi route bach na jaye).
// Badlav (add/edit/hide/restock/category) sirf POST: require_admin -> kaam -> 302 wapas us page par, ?ok=code ya ?err=code ke saath.

// 0 = admin hai, aage badho. Warna jawab bhej diya gaya aur wahi status return hota hai:
// login nahi -> 302 /login, login hai par ADMIN nahi -> 403
static int require_admin(SOCKET client, const Request& req) {
    if (req.user_id == 0) return redirect(client, req, "/login");
    if (req.role != "ADMIN") {
        log_tag("PAGE", "user " + to_string(req.user_id) + " tried " + req.path + " (not an admin)");
        return error_page(client, req, 403, "403 Forbidden");
    }
    return 0;
}

// flash messages: URL me sirf chhota code (?ok=restocked), text yahan fixed hai. Koi bhi text URL se page par nahi aata
struct AdminMsg {
    const char* code;
    const char* text;
};
static const AdminMsg ADMIN_OK[] = {
    {"product_added", "Product added. Its stock starts at 0: use Restock to add stock."},
    {"product_saved", "Product saved."},
    {"product_hidden", "Product hidden from the store."},
    {"product_shown", "Product is visible in the store again."},
    {"restocked", "Stock updated."},
    {"cat_added", "Category added."},
    {"cat_renamed", "Category renamed."},
    {"cat_deleted", "Category deleted."},
    {"order_packed", "Order marked as packed."},
    {"order_shipped", "Order marked as shipped."},
    {"order_ofd", "Order marked as out for delivery."},
    {"order_delivered", "Order delivered. A Cash on Delivery payment is now PAID."},
    {"order_cancelled", "Order cancelled. Its items were added back to stock; an online payment is REFUNDED."},
    {"role_changed", "Role changed. It applies from that user's next request."},
    {"session_ended", "Session ended. That user is logged out."},
    {"sessions_ended", "All active sessions of that user were ended."}};
static const AdminMsg ADMIN_ERR[] = {
    {"stock_below", "Stock cannot go below 0."},
    {"bad_delta", "Stock change must be a whole number like +5 or -2 (not 0)."},
    {"no_product", "Product not found."},
    {"bad_request", "Bad request."},
    {"cat_exists", "That category name already exists."},
    {"cat_has_products", "This category still has products. Move its products to another category first (hiding them is not enough)."},
    {"cat_missing", "Category not found."},
    {"bad_name", "Category name must be 1 to 50 characters."},
    {"own_role", "You cannot change your own role."},
    {"last_admin", "The last admin cannot be demoted: make someone else an admin first."},
    {"same_role", "That user already has this role."},
    {"no_user", "User not found."},
    {"not_admin", "You are no longer an admin."},
    {"session_gone", "That session is already ended or does not exist."},
    {"own_session", "That is your current session: use Sign out instead."},
    {"no_sessions", "That user has no active sessions."},
    {"own_user", "Use Sign out to end your own sessions."}};

static string admin_notice(const Request& req) {
    string ok = param(req, "ok");
    string err = param(req, "err");
    for (const AdminMsg& m : ADMIN_ERR)
        if (err == m.code) return flash_text("err", m.text);
    for (const AdminMsg& m : ADMIN_OK)
        if (ok == m.code) return flash_text("ok", m.text);
    return "";
}

// sidebar ka ek link: active wala indigo
static string admin_link(const string& active, const string& key, const string& href, const string& icon, const string& label) {
    return render("admin_link", {{"on_class", active == key ? " on" : ""}, {"href", href}, {"icon", icon}, {"label", label}});
}

// admin ka poora page: store ka navbar nahi, apni sidebar. active = kaunsa link highlight ho
static int send_admin(SOCKET client, const Request& req, int status, const string& title, const string& active,
                      const string& content, const string& head_extra = "") {
    string side = admin_link(active, "dashboard", "/admin", "📊", "Dashboard") +
                  admin_link(active, "products", "/admin/products", "📦", "Products") +
                  admin_link(active, "categories", "/admin/categories", "🗂️", "Categories") +
                  admin_link(active, "orders", "/admin/orders", "🧾", "Orders") +
                  admin_link(active, "users", "/admin/users", "👥", "Users") +
                  admin_link(active, "sessions", "/admin/sessions", "🔐", "Sessions") +
                  admin_link(active, "logs", "/admin/logs", "📜", "Logs");
    send_response(client, status, HTML,
                  render("admin_layout", {{"title", title}, {"sidebar", side}, {"name", req.user_name},
                                          {"flash", admin_notice(req)}, {"content", content}, {"head_extra", head_extra}}),
                  with_cookie_fix(req, "Cache-Control: no-store\r\n"));
    return status;
}

// admin page jo "nahi mila / galat" bataye (status ke saath)
static int admin_problem(SOCKET client, const Request& req, int status, const string& title, const string& message) {
    return send_admin(client, req, status, title, "", render("admin_message", {{"title", title}, {"message", message}}));
}

// is page ka apna path+query (ok/err hatakar): forms isse "return" me bhejte hain, taaki kaam ke baad yahin wapas aayein
static string admin_self(const Request& req) {
    return strip_param(strip_param(req.target, "ok"), "err");
}

// "return" sirf hamare /admin ke path ho sakte hain (open redirect nahi), warna fallback
static string admin_back(const Request& req, const string& fallback) {
    string r = field(req, "return");
    bool in_admin = r == "/admin" || r.rfind("/admin/", 0) == 0 || r.rfind("/admin?", 0) == 0;
    if (!in_admin || safe_return(r) != r) return fallback;
    return strip_param(strip_param(r, "ok"), "err");
}

// 302 wapas, hare (ok) ya laal (err) message ke saath
static int admin_redirect(SOCKET client, const Request& req, const string& fallback, const string& ok, const string& err) {
    string back = admin_back(req, fallback);
    back += (back.find('?') == string::npos ? "?" : "&") + string(err.empty() ? "ok=" : "err=") + (err.empty() ? ok : err);
    return redirect(client, req, back);
}

// admin badlav ki audit row (db.cpp admin_audit): badlav hone ke baad. Order status aur role ki row transaction ke andar likhi jaati hai
static void audit(const Request& req, const string& action, const string& details) {
    admin_audit(req.user_id, action, details);
}

// POST route par GET aaye (ya ulta): 405
static int admin_method(SOCKET client, const Request& req, const string& want) {
    if (req.method == want) return 0;
    return method_not_allowed(client, req, want);
}

// ---- admin: chhote helpers (validation) ----

// characters ki ginti (bytes nahi): UTF-8 ke beech ke bytes (10xxxxxx) gino mat. VARCHAR(100) characters me naapta hai
static size_t utf8_len(const string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

// "499", "499.5", "499.50" -> paise. Sirf digits aur ek '.', 2 se zyada decimal nahi, 0 < price <= 1,000,000
static bool parse_price(const string& s, long long& cents) {
    size_t dot = s.find('.');
    string whole = s.substr(0, dot);
    string frac = dot == string::npos ? "" : s.substr(dot + 1);
    if (whole.empty() || whole.size() > 7 || frac.size() > 2) return false;
    if (dot != string::npos && frac.empty()) return false;
    for (char c : whole + frac)
        if (c < '0' || c > '9') return false;
    while (frac.size() < 2) frac += '0';
    cents = atoll(whole.c_str()) * 100 + atoll(frac.c_str());
    return cents > 0 && cents <= 100000000LL;
}

// paise -> "1234.50" (DB ke DECIMAL ke liye)
static string cents_plain(long long c) {
    string p = to_string(c % 100);
    return to_string(c / 100) + "." + (p.size() < 2 ? "0" : "") + p;
}

// "+5", "-2", "5" -> "5" / "-2". Poora number, 0 nahi, 100000 se zyada nahi
static bool parse_delta(const string& text, string& out) {
    string s = trim(text);
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = (s[0] == '-');
        s.erase(0, 1);
    }
    if (s.empty() || s.size() > 6) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    int v = atoi(s.c_str());
    if (v == 0 || v > 100000) return false;
    out = (neg ? "-" : "") + to_string(v);
    return true;
}

static string admin_card(const string& label, const string& value, const string& tone = "") {
    return render("admin_card", {{"label", label}, {"value", value}, {"tone", tone}});
}

// chhota "+5 / -2" restock form (dashboard aur products table). back = kaam ke baad wapas kahan
static string restock_form(const string& id, const string& name, const string& back) {
    return render("admin_restock_form", {{"id", id}, {"name", name}, {"back", back}});
}

// ---- admin: dashboard ----

// GET /admin: saari queries EK pooled connection par (get_conn shuru me, put_conn page banane se pehle)
static int admin_dashboard_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return error_page(client, req, 500, "Database error");

    const string low = to_string(LOW_STOCK);
    vector<vector<string>> stats, recent, by_status, low_rows;
    bool ok =
        db_select_on(m, "SELECT COALESCE((SELECT SUM(total) FROM orders WHERE status <> 'CANCELLED'), 0), "
                        "(SELECT COUNT(*) FROM orders), (SELECT COUNT(*) FROM orders WHERE status = 'PLACED'), "
                        "(SELECT COUNT(*) FROM users), (SELECT COUNT(*) FROM products), "
                        "(SELECT COUNT(*) FROM products WHERE stock <= ? AND is_active = TRUE), "
                        "(SELECT COUNT(*) FROM user_sessions WHERE is_active = 1 AND expires_at > NOW())",
                     {low}, stats) &&
        db_select_on(m, "SELECT o.order_id, u.name, o.created_at, o.total, o.status FROM orders o "
                        "JOIN users u ON u.user_id = o.user_id ORDER BY o.order_id DESC LIMIT 10", {}, recent) &&
        db_select_on(m, "SELECT status, COUNT(*) FROM orders GROUP BY status ORDER BY COUNT(*) DESC, status", {}, by_status) &&
        db_select_on(m, "SELECT product_id, name, stock FROM products WHERE stock <= ? AND is_active = TRUE "
                        "ORDER BY stock, name LIMIT 20", {low}, low_rows);
    put_conn(id);                       // ab connection ki zaroorat nahi: page banana DB ke bina
    if (!ok || stats.size() != 1 || !cols_ok(stats, 7) || !cols_ok(by_status, 2) || !cols_ok(low_rows, 3) ||
        (!recent.empty() && !cols_ok(recent, 5)))
        return error_page(client, req, 500, "Database error");

    vector<string>& s = stats[0];
    string cards = admin_card("Revenue (excl. cancelled)", rupees_text(s[0])) + admin_card("Total orders", s[1]) +
                   admin_card("Orders waiting (PLACED)", s[2], s[2] != "0" ? "warn" : "") + admin_card("Users", s[3]) +
                   admin_card("Products", s[4]) +
                   admin_card("Low stock (<= " + low + ")", s[5], s[5] != "0" ? "err" : "") + admin_card("Active sessions", s[6]);

    string orders;
    for (auto& r : recent)
        orders += render("admin_order_row", {{"id", r[0]}, {"customer", r[1]}, {"date", r[2]}, {"total", rupees_text(r[3])},
                                             {"status", status_label(r[4])}, {"status_class", status_class(r[4])}});
    if (recent.empty()) orders = render("admin_none_row", {{"message", "No orders yet."}});
    string statuses;
    for (auto& r : by_status)
        statuses += render("admin_status_count", {{"status", status_label(r[0])}, {"status_class", status_class(r[0])}, {"count", r[1]}});

    string low_html;
    if (low_rows.empty()) {
        low_html = render("admin_none", {{"message", "Every product has more than " + low + " in stock."}});
    } else {
        string rows;
        for (auto& r : low_rows)
            rows += render("admin_low_row", {{"name", r[1]}, {"stock", r[2]}, {"tone", r[2] == "0" ? "pill-cancelled" : "pill-placed"},
                                             {"restock", restock_form(r[0], r[1], "/admin")}});
        low_html = render("admin_low", {{"rows", rows}});
    }
    return send_admin(client, req, 200, "Dashboard", "dashboard",
                      render("admin_dashboard", {{"cards", cards}, {"orders", orders}, {"statuses", statuses}, {"low", low_html}}));
}

// ---- admin: products ----

// GET /admin/products?q=&cat=&hidden=1
static int admin_products_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string q = trim(param(req, "q"));
    string cat = param(req, "cat");
    bool show_hidden = param(req, "hidden") == "1";
    if (!cat.empty() && !is_number(cat)) return admin_problem(client, req, 400, "Products", "Bad category filter.");

    // SQL ke tukde fixed; user ka data sirf args me (?)
    string sql = "SELECT p.product_id, p.name, c.category_name, p.price, p.stock, p.is_active FROM products p "
                 "JOIN categories c ON c.category_id = p.category_id WHERE 1=1";
    vector<string> args;
    if (!show_hidden) sql += " AND p.is_active = TRUE";
    if (!cat.empty()) {
        sql += " AND p.category_id = ?";
        args.push_back(cat);
    }
    if (!q.empty()) {
        sql += " AND p.name LIKE ? ESCAPE '!'";
        args.push_back("%" + escape_like(q) + "%");
    }
    sql += " ORDER BY p.product_id LIMIT 300";
    vector<vector<string>> prods;
    if (!db_select(sql, args, prods) || !cols_ok(prods, 6)) return error_page(client, req, 500, "Database error");

    string back = admin_self(req);
    string rows;
    for (auto& p : prods) {
        bool active = (p[5] != "0");
        rows += render("admin_product_row", {{"row_class", active ? "" : "dim"}, {"id", p[0]}, {"name", p[1]}, {"category", p[2]},
                                             {"price", rupees_text(p[3])}, {"stock", p[4]},
                                             {"state", active ? "active" : "hidden"}, {"state_label", active ? "Active" : "Hidden"},
                                             {"new_active", active ? "0" : "1"}, {"toggle_label", active ? "Hide" : "Show"},
                                             {"restock", restock_form(p[0], p[1], back)}, {"back", back}});
    }
    if (prods.empty()) rows = render("admin_none_row", {{"message", "No products match."}});

    string options = render("search_option", {{"value", ""}, {"label", "All categories"}, {"selected", cat.empty() ? "selected" : ""}});
    for (auto& c : get_categories(true))
        options += render("search_option", {{"value", c[0]}, {"label", c[1]}, {"selected", c[0] == cat ? "selected" : ""}});
    string count = to_string(prods.size()) + (prods.size() == 1 ? " product" : " products") +
                   (prods.size() == 300 ? " (showing the first 300: use the filters)" : "");
    return send_admin(client, req, 200, "Products", "products",
                      render("admin_products", {{"q", q}, {"options", options}, {"hidden_checked", show_hidden ? "checked" : ""},
                                                {"count", count}, {"rows", rows}}));
}

// add / edit form ke fields (jaise user ne bhare)
struct ProductForm {
    string id, name, description, price, category;
};

static ProductForm read_product_form(const Request& req) {
    ProductForm f;
    f.id = trim(field(req, "product_id"));
    f.name = trim(field(req, "name"));
    f.description = trim(field(req, "description"));
    f.price = trim(field(req, "price"));
    f.category = trim(field(req, "category_id"));
    return f;
}

// "" = sab theek (cents me price bhi), warna user ko dikhane layak galti
static string check_product_form(const ProductForm& f, long long& cents) {
    size_t n = utf8_len(f.name);
    if (n < 1 || n > 100) return "Name must be 1 to 100 characters.";
    if (utf8_len(f.description) > 255) return "Description can be at most 255 characters.";
    if (!parse_price(f.price, cents)) return "Price must be more than 0 and at most 1000000 (at most 2 decimals).";
    if (!is_number(f.category)) return "Please choose a category.";
    vector<vector<string>> rows;
    if (!db_select("SELECT category_id FROM categories WHERE category_id = ?", {f.category}, rows)) return "Database error";
    if (rows.empty()) return "That category does not exist.";
    return "";
}

static int product_form_page(SOCKET client, const Request& req, int status, bool is_new, const ProductForm& f, const string& error) {
    string options;
    for (auto& c : get_categories(true))
        options += render("search_option", {{"value", c[0]}, {"label", c[1]}, {"selected", c[0] == f.category ? "selected" : ""}});
    string heading = is_new ? "New product" : "Edit product";
    string content = render("admin_product_form", {
        {"heading", heading}, {"action", is_new ? "/admin/products/new" : "/admin/products/edit"},
        {"error", error.empty() ? "" : flash_text("err", error)},
        {"id_field", f.id.empty() ? "" : render("hidden_input", {{"name", "product_id"}, {"value", f.id}})},
        {"name", f.name}, {"description", f.description}, {"price", f.price}, {"options", options},
        {"note", is_new ? "Stock starts at 0. Use Restock after saving." : "Stock is not changed here: use Restock on the products list."},
        {"submit", is_new ? "Add product" : "Save changes"}});
    return send_admin(client, req, status, heading, "products", content);
}

// GET /admin/products/new (form) aur POST (create)
static int admin_product_new_page(SOCKET client, const Request& req) {
    if (req.method != "POST") {
        if (req.method != "GET") return method_not_allowed(client, req, "GET, POST");
        return product_form_page(client, req, 200, true, ProductForm(), "");
    }
    ProductForm f = read_product_form(req);
    long long cents = 0;
    string err = check_product_form(f, cents);
    if (!err.empty()) return product_form_page(client, req, err == "Database error" ? 500 : 400, true, f, err);

    int n = db_exec("INSERT INTO products (name, description, price, stock, category_id) VALUES (?, ?, ?, 0, ?)",
                    {f.name, f.description, cents_plain(cents), f.category});
    if (n < 0) return product_form_page(client, req, 500, true, f, "Database error");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " added product '" + f.name + "'");
    audit(req, "PRODUCT_ADD", "product '" + f.name + "' price " + cents_plain(cents) + " category " + f.category);
    return admin_redirect(client, req, "/admin/products", "product_added", "");
}

// GET /admin/products/edit?id=N (form) aur POST (update: stock nahi badalta)
static int admin_product_edit_page(SOCKET client, const Request& req) {
    if (req.method == "GET") {
        string id = param(req, "id");
        if (!valid_product_id(id)) return admin_problem(client, req, 400, "Edit product", "Bad product id.");
        vector<vector<string>> rows;
        if (!db_select("SELECT product_id, name, description, price, category_id FROM products WHERE product_id = ?", {id}, rows) ||
            !cols_ok(rows, 5))
            return error_page(client, req, 500, "Database error");
        if (rows.empty()) return admin_problem(client, req, 404, "Edit product", "Product not found.");
        ProductForm f;
        f.id = rows[0][0]; f.name = rows[0][1]; f.description = rows[0][2]; f.price = rows[0][3]; f.category = rows[0][4];
        return product_form_page(client, req, 200, false, f, "");
    }
    if (req.method != "POST") return method_not_allowed(client, req, "GET, POST");

    ProductForm f = read_product_form(req);
    if (!valid_product_id(f.id)) return admin_redirect(client, req, "/admin/products", "", "bad_request");
    long long cents = 0;
    string err = check_product_form(f, cents);
    if (!err.empty()) return product_form_page(client, req, err == "Database error" ? 500 : 400, false, f, err);

    // stock is query me nahi hai: edit se stock kabhi nahi badalta (restock ka kaam alag, atomic)
    int n = db_exec("UPDATE products SET name = ?, description = ?, price = ?, category_id = ? WHERE product_id = ?",
                    {f.name, f.description, cents_plain(cents), f.category, f.id});
    if (n < 0) return product_form_page(client, req, 500, false, f, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/products", "", "no_product");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " edited product " + f.id);
    audit(req, "PRODUCT_EDIT", "product " + f.id + " '" + f.name + "' price " + cents_plain(cents) + " category " + f.category);
    return admin_redirect(client, req, "/admin/products", "product_saved", "");
}

// POST /admin/products/restock (product_id, delta "+5" / "-2"): EK atomic query.
// "stock = stock + ?" (kabhi "stock = <number>" nahi): usi pal koi sale ho rahi ho to uska ghatna mit na jaye.
// "AND stock + ? >= 0": zero se neeche jaane wala change DB hi mana kar deta hai (0 rows)
static int admin_restock_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string pid = trim(field(req, "product_id"));
    string delta;
    if (!valid_product_id(pid)) return admin_redirect(client, req, "/admin/products", "", "bad_request");
    if (!parse_delta(field(req, "delta"), delta)) return admin_redirect(client, req, "/admin/products", "", "bad_delta");

    int n = db_exec("UPDATE products SET stock = stock + CAST(? AS SIGNED) "
                    "WHERE product_id = ? AND stock + CAST(? AS SIGNED) >= 0", {delta, pid, delta});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) {                       // product hi nahi hai, ya stock zero se neeche jaata
        vector<vector<string>> rows;
        if (!db_select("SELECT 1 FROM products WHERE product_id = ?", {pid}, rows)) return error_page(client, req, 500, "Database error");
        return admin_redirect(client, req, "/admin/products", "", rows.empty() ? "no_product" : "stock_below");
    }
    log_tag("ADMIN", "user " + to_string(req.user_id) + " restock product " + pid + " by " + delta);
    audit(req, "PRODUCT_RESTOCK", "product " + pid + " by " + delta);
    return admin_redirect(client, req, "/admin/products", "restocked", "");
}

// POST /admin/products/toggle (product_id, active = 0 hide / 1 show). Product DELETE nahi hota: purane orders use point karte hain
static int admin_toggle_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string pid = trim(field(req, "product_id"));
    string active = field(req, "active");
    if (!valid_product_id(pid) || (active != "0" && active != "1"))
        return admin_redirect(client, req, "/admin/products", "", "bad_request");
    int n = db_exec("UPDATE products SET is_active = ? WHERE product_id = ?", {active, pid});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/products", "", "no_product");
    log_tag("ADMIN", "user " + to_string(req.user_id) + (active == "1" ? " showed" : " hid") + " product " + pid);
    audit(req, active == "1" ? "PRODUCT_SHOW" : "PRODUCT_HIDE", "product " + pid);
    return admin_redirect(client, req, "/admin/products", active == "1" ? "product_shown" : "product_hidden", "");
}

// ---- admin: categories ----

// GET /admin/categories: har category aur kitne products (LEFT JOIN: khali category bhi 0 ke saath dikhe)
static int admin_categories_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    vector<vector<string>> cats;
    if (!db_select("SELECT c.category_id, c.category_name, COUNT(p.product_id) FROM categories c "
                   "LEFT JOIN products p ON p.category_id = c.category_id "
                   "GROUP BY c.category_id, c.category_name ORDER BY c.category_id", {}, cats) ||
        !cols_ok(cats, 3))
        return error_page(client, req, 500, "Database error");
    string rows;
    for (auto& c : cats)
        rows += render("admin_category_row", {{"id", c[0]}, {"name", c[1]}, {"count", c[2]}});
    if (cats.empty()) rows = render("admin_none_row", {{"message", "No categories yet."}});
    return send_admin(client, req, 200, "Categories", "categories", render("admin_categories", {{"rows", rows}}));
}

// categories badalte hi navbar ki purani list hata do: store me agli request se naya naam dikhe
static void clear_category_cache() {
    cat_lock.lock();
    cat_cache.clear();
    cat_loaded_us = 0;
    cat_lock.unlock();
}

// category ke naam: 1 se 50 characters (VARCHAR(50)); "" = galat
static string clean_category_name(const Request& req) {
    string name = trim(field(req, "name"));
    size_t n = utf8_len(name);
    return (n < 1 || n > 50) ? "" : name;
}

// MySQL error number -> admin message code (sirf categories ke liye)
static int category_result(SOCKET client, const Request& req, int n, int code, const string& ok_code,
                           const string& audit_action, const string& details) {
    if (n < 0 && code == 1062) return admin_redirect(client, req, "/admin/categories", "", "cat_exists");
    if (n < 0 && code == 1451) return admin_redirect(client, req, "/admin/categories", "", "cat_has_products");
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/categories", "", "cat_missing");
    clear_category_cache();
    log_tag("ADMIN", "user " + to_string(req.user_id) + " category change: " + ok_code);
    audit(req, audit_action, details);
    return admin_redirect(client, req, "/admin/categories", ok_code, "");
}

static int admin_category_add_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string name = clean_category_name(req);
    if (name.empty()) return admin_redirect(client, req, "/admin/categories", "", "bad_name");
    int code = 0;
    int n = db_exec("INSERT INTO categories (category_name) VALUES (?)", {name}, &code);
    return category_result(client, req, n, code, "cat_added", "CATEGORY_ADD", "category '" + name + "'");
}

static int admin_category_rename_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "category_id"));
    string name = clean_category_name(req);
    if (!is_number(id)) return admin_redirect(client, req, "/admin/categories", "", "bad_request");
    if (name.empty()) return admin_redirect(client, req, "/admin/categories", "", "bad_name");
    int code = 0;
    int n = db_exec("UPDATE categories SET category_name = ? WHERE category_id = ?", {name, id}, &code);
    return category_result(client, req, n, code, "cat_renamed", "CATEGORY_RENAME", "category " + id + " renamed to '" + name + "'");
}

// DELETE sirf khali category ka: products ho to FK (1451) rok deta hai
static int admin_category_delete_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "category_id"));
    if (!is_number(id)) return admin_redirect(client, req, "/admin/categories", "", "bad_request");
    int code = 0;
    int n = db_exec("DELETE FROM categories WHERE category_id = ?", {id}, &code);
    return category_result(client, req, n, code, "cat_deleted", "CATEGORY_DELETE", "category " + id);
}

// ---- admin: orders (A3) ----

// link me query text daalne ke liye: sirf A-Z a-z 0-9 - _ . ~ jaise hain, baaki %XX
static string url_encode(const string& s) {
    static const char* HEX = "0123456789ABCDEF";
    string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        }
    }
    return out;
}

// Newer / Older links (filters base me), kuch na ho to khali
static string admin_pager_html(const string& base, int pg, bool more) {
    string prev = pg > 1 ? render("admin_pager_link", {{"href", base + "page=" + to_string(pg - 1)}, {"label", "&larr; Newer"}}) : "";
    string next = more ? render("admin_pager_link", {{"href", base + "page=" + to_string(pg + 1)}, {"label", "Older &rarr;"}}) : "";
    if (prev.empty() && next.empty()) return "";
    return render("admin_pager", {{"prev", prev}, {"next", next}, {"page", to_string(pg)}});
}

// order statuses (sirf jo DB ka CHECK allow karta hai): filter aur status badalne ke form ki jaanch ke liye
static bool known_order_status(const string& s) {
    static const char* ALL[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED"};
    for (const char* a : ALL)
        if (s == a) return true;
    return false;
}

const int ORDERS_PER_PAGE = 20;

// GET /admin/orders?status=&q=&page=
static int admin_orders_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string status = param(req, "status");
    string q = trim(param(req, "q"));
    string page_text = param(req, "page");
    if (!status.empty() && !known_order_status(status)) return admin_problem(client, req, 400, "Orders", "Bad status filter.");
    int pg = 1;
    if (!page_text.empty() && !parse_range(page_text, 1, 100000, pg)) return admin_problem(client, req, 400, "Orders", "Bad page number.");

    // SQL ke tukde fixed; filter ki values sirf args me. LIMIT/OFFSET ke number hamare apne ints hain (user ka text nahi)
    string sql = "SELECT o.order_id, u.name, u.email, o.created_at, "
                 "(SELECT COALESCE(SUM(oi.quantity), 0) FROM order_items oi WHERE oi.order_id = o.order_id), "
                 "o.total, o.status, IFNULL(p.status, '-') "
                 "FROM orders o JOIN users u ON u.user_id = o.user_id LEFT JOIN payments p ON p.order_id = o.order_id WHERE 1=1";
    vector<string> args;
    if (!status.empty()) {
        sql += " AND o.status = ?";
        args.push_back(status);
    }
    if (!q.empty()) {
        sql += " AND (o.order_id = ? OR u.email LIKE ? ESCAPE '!')";
        args.push_back(is_number(q) ? q : "0");              // number ho to order id bhi; order id 0 hota nahi
        args.push_back("%" + escape_like(q) + "%");
    }
    sql += " ORDER BY o.order_id DESC LIMIT " + to_string(ORDERS_PER_PAGE + 1) + " OFFSET " + to_string((pg - 1) * ORDERS_PER_PAGE);

    vector<vector<string>> found;
    if (!db_select(sql, args, found) || !cols_ok(found, 8)) return error_page(client, req, 500, "Database error");
    bool more = (int)found.size() > ORDERS_PER_PAGE;         // ek extra row mili = agla page hai
    if (more) found.resize(ORDERS_PER_PAGE);

    string rows;
    for (auto& r : found)
        rows += render("admin_orders_row", {{"id", r[0]}, {"name", r[1]}, {"email", r[2]}, {"date", r[3]}, {"items", r[4]},
                                            {"total", rupees_text(r[5])}, {"status", status_label(r[6])}, {"status_class", status_class(r[6])},
                                            {"pay", r[7]}, {"pay_class", status_class(r[7])}});
    if (found.empty()) rows = render("admin_none_row", {{"message", "No orders match."}});

    string options = render("search_option", {{"value", ""}, {"label", "All statuses"}, {"selected", status.empty() ? "selected" : ""}});
    for (const char* s : {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED", "CANCELLED"})
        options += render("search_option", {{"value", s}, {"label", status_label(s)}, {"selected", status == s ? "selected" : ""}});

    // pager: filters link me saath rehte hain
    string base = "/admin/orders?";
    if (!status.empty()) base += "status=" + url_encode(status) + "&";
    if (!q.empty()) base += "q=" + url_encode(q) + "&";
    string pager = admin_pager_html(base, pg, more);

    string count = to_string(found.size()) + (found.size() == 1 ? " order" : " orders") + " on this page (newest first, " +
                   to_string(ORDERS_PER_PAGE) + " per page)";
    return send_admin(client, req, 200, "Orders", "orders",
                      render("admin_orders", {{"options", options}, {"q", q}, {"count", count}, {"rows", rows}, {"pager", pager}}));
}

// GET /admin/order?id=N: customer, items (price at purchase), payment, aur sirf allowed next statuses ke buttons
static int admin_order_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string id = param(req, "id");
    if (!valid_product_id(id)) return admin_problem(client, req, 400, "Order", "Bad order id.");

    vector<vector<string>> head, items, hist;
    int cid = 0;
    MYSQL* m = get_conn(cid);               // teeno queries ek connection par
    if (m == nullptr) return error_page(client, req, 500, "Database error");
    bool ok = db_select_on(m, "SELECT o.order_id, o.status, o.total, o.created_at, u.name, u.email, IFNULL(p.method, ''), "
                              "IFNULL(p.status, ''), IFNULL(p.paid_at, ''), IFNULL(p.pay_detail, ''), IFNULL(p.txn_ref, '') "
                              "FROM orders o JOIN users u ON u.user_id = o.user_id "
                              "LEFT JOIN payments p ON p.order_id = o.order_id WHERE o.order_id = ?", {id}, head) &&
              db_select_on(m, "SELECT p.product_id, p.name, oi.quantity, oi.price FROM order_items oi "
                              "JOIN products p ON p.product_id = oi.product_id WHERE oi.order_id = ? ORDER BY p.name", {id}, items) &&
              db_select_on(m, "SELECT status, changed_at, changed_by FROM order_status_history WHERE order_id = ? ORDER BY history_id",
                           {id}, hist);
    put_conn(cid);
    if (!ok) return error_page(client, req, 500, "Database error");
    if (head.empty()) return admin_problem(client, req, 404, "Order", "Order not found.");
    if (!cols_ok(head, 11) || !cols_ok(items, 4) || !cols_ok(hist, 3)) return error_page(client, req, 500, "Database error");

    vector<string>& o = head[0];
    string rows;
    for (auto& r : items)
        rows += render("admin_order_item_row", {{"name", r[1]}, {"qty", r[2]}, {"price", rupees_text(r[3])},
                                                {"subtotal", rupees(to_cents(r[3]) * atoi(r[2].c_str()))}});
    string addr;
    if (!order_address_text(o[0], addr)) return error_page(client, req, 500, "Database error");
    string history;                         // poori history: kab, kaun sa status, kisne badla (SYSTEM / ADMIN / CUSTOMER)
    for (auto& h : hist)
        history += render("admin_history_row", {{"time", h[1]}, {"status_class", status_class(h[0])}, {"label", status_label(h[0])}, {"by", h[2]}});
    if (hist.empty()) history = render("admin_none_row", {{"message", "No history yet (order placed before tracking was added)."}});
    string pay = payment_html(o[6], o[9], o[7], o[8], o[10]);

    // buttons: har possible "to" ke liye, agar db.cpp ka order_transition_ok haan kahe (wahi niyam jo server lagata hai)
    struct Next {
        const char* to;
        const char* label;
        const char* tone;
    };
    static const Next NEXT[] = {{"PACKED", "Mark as packed", "btn-primary"},
                                {"SHIPPED", "Mark as shipped", "btn-primary"},
                                {"OUT_FOR_DELIVERY", "Mark as out for delivery", "btn-primary"},
                                {"DELIVERED", "Mark as delivered (payment becomes PAID)", "btn-primary"},
                                {"CANCELLED", "Cancel order (items go back to stock)", "btn-danger"}};
    string buttons;
    for (const Next& n : NEXT)
        if (order_transition_ok(o[1], n.to))
            buttons += render("admin_order_action", {{"id", o[0]}, {"to", n.to}, {"from", o[1]}, {"label", n.label}, {"tone", n.tone}});
    string actions = buttons.empty() ? render("admin_none", {{"message", "This order is final: " + status_label(o[1]) + " orders cannot be changed."}})
                                     : render("admin_order_buttons", {{"buttons", buttons}});

    return send_admin(client, req, 200, "Order #" + o[0], "orders",
                      render("admin_order", {{"id", o[0]}, {"status", status_label(o[1])}, {"status_class", status_class(o[1])}, {"total", rupees_text(o[2])},
                                             {"placed", o[3]}, {"name", o[4]}, {"email", o[5]}, {"payment", pay}, {"rows", rows},
                                             {"history", history}, {"address", addr}, {"actions", actions}}));
}

// POST /admin/order/status (order_id, to, from = status jo admin ne page par dekha). Asli kaam db.cpp ki order_change
static int admin_order_status_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "order_id"));
    string to = trim(field(req, "to"));
    string from = trim(field(req, "from"));
    if (!valid_product_id(id) || !known_order_status(to) || to == "PLACED" || (!from.empty() && !known_order_status(from)))
        return admin_redirect(client, req, "/admin/orders", "", "bad_request");

    OrderResult r = order_change(atoi(id.c_str()), to, req.user_id, true, from);
    string back = "/admin/order?id=" + id;
    if (r.status == 200) {
        log_tag("ADMIN", "user " + to_string(req.user_id) + " order " + id + " " + r.from + "->" + to);
        string code = to == "PACKED" ? "order_packed" : to == "SHIPPED" ? "order_shipped" : to == "OUT_FOR_DELIVERY" ? "order_ofd" :
                      to == "DELIVERED" ? "order_delivered" : "order_cancelled";
        return admin_redirect(client, req, back, code, "");
    }
    if (r.status == 400 || r.status == 404 || r.status == 409 || r.status == 503)
        return admin_problem(client, req, r.status, "Order #" + id, r.message);
    return error_page(client, req, 500, "Database error");
}

// ---- admin: users (A4) ----

// GET /admin/users?q=&page=
static int admin_users_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string q = trim(param(req, "q"));
    string page_text = param(req, "page");
    int pg = 1;
    if (!page_text.empty() && !parse_range(page_text, 1, 100000, pg)) return admin_problem(client, req, 400, "Users", "Bad page number.");

    string sql = "SELECT u.user_id, u.name, u.email, u.role, u.created_at, "
                 "(SELECT COUNT(*) FROM orders o WHERE o.user_id = u.user_id), "
                 "(SELECT COUNT(*) FROM user_sessions s WHERE s.user_id = u.user_id AND s.is_active = 1 AND s.expires_at > NOW()) "
                 "FROM users u WHERE 1=1";
    vector<string> args;
    if (!q.empty()) {
        sql += " AND (u.name LIKE ? ESCAPE '!' OR u.email LIKE ? ESCAPE '!')";
        args.push_back("%" + escape_like(q) + "%");
        args.push_back("%" + escape_like(q) + "%");
    }
    sql += " ORDER BY u.user_id LIMIT " + to_string(ORDERS_PER_PAGE + 1) + " OFFSET " + to_string((pg - 1) * ORDERS_PER_PAGE);

    vector<vector<string>> found;
    if (!db_select(sql, args, found) || !cols_ok(found, 7)) return error_page(client, req, 500, "Database error");
    bool more = (int)found.size() > ORDERS_PER_PAGE;
    if (more) found.resize(ORDERS_PER_PAGE);

    string rows;
    for (auto& u : found) {
        bool is_admin = (u[3] == "ADMIN");
        bool me = (atoi(u[0].c_str()) == req.user_id);
        string actions;
        if (me) actions = render("admin_user_me", {});           // apna role / sessions yahan se nahi
        else {
            actions = render("admin_role_form", {{"id", u[0]}, {"role", is_admin ? "CUSTOMER" : "ADMIN"},
                                                 {"label", is_admin ? "Make customer" : "Make admin"}, {"back", admin_self(req)}});
            if (u[6] != "0") actions += " " + render("admin_logout_all_form", {{"id", u[0]}, {"back", admin_self(req)}});
        }
        rows += render("admin_user_row", {{"id", u[0]}, {"name", u[1]}, {"email", u[2]}, {"role", u[3]}, {"role_class", status_class(u[3])},
                                          {"joined", u[4]}, {"orders", u[5]}, {"sessions", u[6]}, {"actions", actions}});
    }
    if (found.empty()) rows = render("admin_none_row", {{"message", "No users match."}});

    string base = "/admin/users?";
    if (!q.empty()) base += "q=" + url_encode(q) + "&";
    string count = to_string(found.size()) + (found.size() == 1 ? " user" : " users") + " on this page (" + to_string(ORDERS_PER_PAGE) + " per page)";
    return send_admin(client, req, 200, "Users", "users",
                      render("admin_users", {{"q", q}, {"count", count}, {"rows", rows}, {"pager", admin_pager_html(base, pg, more)}}));
}

// POST /admin/users/role (user_id, role): niyam db.cpp ki role_change me (ek transaction)
static int admin_user_role_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "user_id"));
    string role = trim(field(req, "role"));
    if (!valid_product_id(id) || (role != "ADMIN" && role != "CUSTOMER")) return admin_redirect(client, req, "/admin/users", "", "bad_request");

    RoleResult r = role_change(req.user_id, atoi(id.c_str()), role);
    if (r.status == 200) {
        log_tag("ADMIN", "user " + to_string(req.user_id) + " changed role of user " + id + " " + r.from + "->" + role);
        return admin_redirect(client, req, "/admin/users", "role_changed", "");
    }
    if (r.status >= 500 && r.code != "busy") return error_page(client, req, 500, "Database error");
    if (r.code == "busy") return error_page(client, req, 503, "The shop is busy right now. Please try again in a moment.");
    return admin_redirect(client, req, "/admin/users", "", r.code);       // own_role, last_admin, same_role, no_user, not_admin
}

// ---- admin: sessions (A4) ----

// admin ke liye status: Active / Logged out / Expired / Ended by admin (session_status upar customer page ke saath common hai)
static string admin_session_state(const vector<string>& s) {
    return session_status(s[6], s[7], s[8]);       // is_active, expired, end_reason
}

// GET /admin/sessions?show=active|all&page=
static int admin_sessions_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string show = param(req, "show");
    if (show.empty()) show = "active";
    if (show != "active" && show != "all") return admin_problem(client, req, 400, "Sessions", "Bad filter.");
    string page_text = param(req, "page");
    int pg = 1;
    if (!page_text.empty() && !parse_range(page_text, 1, 100000, pg)) return admin_problem(client, req, 400, "Sessions", "Bad page number.");

    string sql = "SELECT s.session_id, u.name, u.email, s.login_time, s.last_activity, s.expires_at, s.is_active, s.expires_at <= NOW(), "
                 "IFNULL(s.end_reason, '') FROM user_sessions s JOIN users u ON u.user_id = s.user_id";
    if (show == "active") sql += " WHERE s.is_active = 1 AND s.expires_at > NOW()";
    sql += " ORDER BY s.login_time DESC, s.session_id LIMIT " + to_string(ORDERS_PER_PAGE + 1) + " OFFSET " + to_string((pg - 1) * ORDERS_PER_PAGE);

    vector<vector<string>> found;
    if (!db_select(sql, {}, found) || !cols_ok(found, 9)) return error_page(client, req, 500, "Database error");
    bool more = (int)found.size() > ORDERS_PER_PAGE;
    if (more) found.resize(ORDERS_PER_PAGE);

    string rows;
    for (auto& s : found) {
        string state = admin_session_state(s);
        bool mine = (s[0] == req.session_id);
        // poori session id kabhi page par nahi: sirf pehle 8 characters (button bhi inhi 8 se session dhundta hai)
        string short_id = s[0].substr(0, 8);
        string action = state == "Active" && !mine ? render("admin_end_session_form", {{"id", short_id}, {"back", admin_self(req)}}) : (mine ? render("admin_user_me", {}) : "");
        rows += render("admin_session_row", {{"session", short_id}, {"name", s[1]}, {"email", s[2]}, {"login", s[3]}, {"activity", s[4]},
                                             {"expires", s[5]}, {"state", state}, {"state_class", status_class(state)}, {"action", action}});
    }
    if (found.empty()) rows = render("admin_none_row", {{"message", "No sessions to show."}});

    string base = "/admin/sessions?show=" + show + "&";
    string tabs = render("admin_tab", {{"href", "/admin/sessions?show=active"}, {"label", "Active"}, {"on", show == "active" ? " on" : ""}}) +
                  render("admin_tab", {{"href", "/admin/sessions?show=all"}, {"label", "All"}, {"on", show == "all" ? " on" : ""}});
    return send_admin(client, req, 200, "Sessions", "sessions",
                      render("admin_sessions", {{"tabs", tabs}, {"rows", rows}, {"pager", admin_pager_html(base, pg, more)}}));
}

// POST /admin/sessions/end (session_id = pehle 8 hex characters, ya poori 64): session band, end_reason 'FORCED'
static int admin_session_end_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string sid = trim(field(req, "session_id"));
    bool hex = true;
    for (char c : sid)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = false;
    if (!hex || (sid.size() != 8 && sid.size() != 64)) return admin_redirect(client, req, "/admin/sessions", "", "bad_request");

    // 8 characters se EK hi active session milna chahiye (poori id page par kabhi nahi dikhai)
    vector<vector<string>> found;
    if (!db_select("SELECT session_id FROM user_sessions WHERE session_id LIKE ? AND is_active = 1 LIMIT 2", {sid + "%"}, found))
        return error_page(client, req, 500, "Database error");
    if (found.empty()) return admin_redirect(client, req, "/admin/sessions", "", "session_gone");
    if (found.size() > 1) return admin_redirect(client, req, "/admin/sessions", "", "bad_request");
    const string full = found[0][0];
    if (full == req.session_id) return admin_redirect(client, req, "/admin/sessions", "", "own_session");

    int n = db_exec("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED' WHERE session_id = ? AND is_active = 1", {full});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/sessions", "", "session_gone");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " ended session " + full.substr(0, 8));
    audit(req, "SESSION_END", "session " + full.substr(0, 8));
    return admin_redirect(client, req, "/admin/sessions", "session_ended", "");
}

// POST /admin/users/logout-all (user_id): us user ke saare active sessions band
static int admin_user_logout_all_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "POST") != 0) return 405;
    string id = trim(field(req, "user_id"));
    if (!valid_product_id(id)) return admin_redirect(client, req, "/admin/users", "", "bad_request");
    if (atoi(id.c_str()) == req.user_id) return admin_redirect(client, req, "/admin/users", "", "own_user");

    int n = db_exec("UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'FORCED' WHERE user_id = ? AND is_active = 1", {id});
    if (n < 0) return error_page(client, req, 500, "Database error");
    if (n == 0) return admin_redirect(client, req, "/admin/users", "", "no_sessions");
    log_tag("ADMIN", "user " + to_string(req.user_id) + " ended all " + to_string(n) + " sessions of user " + id);
    audit(req, "USER_LOGOUT_ALL", "user " + id + ", " + to_string(n) + " sessions ended");
    return admin_redirect(client, req, "/admin/users", "sessions_ended", "");
}

// ---- admin: logs (A4) ----

// GET /admin/logs?worker=&cls=2xx|3xx|4xx|5xx&path=&refresh=1
static int admin_logs_page(SOCKET client, const Request& req) {
    if (admin_method(client, req, "GET") != 0) return 405;
    string worker = trim(param(req, "worker"));
    string cls = param(req, "cls");
    string path = trim(param(req, "path"));
    bool refresh = param(req, "refresh") == "1";
    if (!worker.empty() && !is_number(worker)) return admin_problem(client, req, 400, "Logs", "Bad worker filter.");
    if (!cls.empty() && cls != "2xx" && cls != "3xx" && cls != "4xx" && cls != "5xx") return admin_problem(client, req, 400, "Logs", "Bad status filter.");

    // SQL ke tukde fixed; worker aur path sirf args me. Status class ke number hamare apne (2xx -> 200..299)
    string sql = "SELECT l.created_at, l.method, l.path, l.status, l.latency_ms, l.queue_wait_ms, l.worker_id, "
                 "IFNULL(CONCAT(u.name, ' (#', l.user_id, ')'), '-') FROM request_logs l LEFT JOIN users u ON u.user_id = l.user_id WHERE 1=1";
    vector<string> args;
    if (!worker.empty()) {
        sql += " AND l.worker_id = ?";
        args.push_back(worker);
    }
    if (!cls.empty()) {
        int lo = (cls[0] - '0') * 100;
        sql += " AND l.status BETWEEN " + to_string(lo) + " AND " + to_string(lo + 99);
    }
    if (!path.empty()) {
        sql += " AND l.path LIKE ? ESCAPE '!'";
        args.push_back("%" + escape_like(path) + "%");
    }
    sql += " ORDER BY l.log_id DESC LIMIT 100";

    vector<vector<string>> logs, workers, actions;
    int cid = 0;
    MYSQL* m = get_conn(cid);               // teeno queries ek connection par
    if (m == nullptr) return error_page(client, req, 500, "Database error");
    bool ok = db_select_on(m, sql, args, logs) &&
              db_select_on(m, "SELECT worker_id, COUNT(*), AVG(latency_ms) FROM request_logs "
                              "WHERE created_at >= NOW() - INTERVAL 5 MINUTE GROUP BY worker_id ORDER BY worker_id", {}, workers) &&
              db_select_on(m, "SELECT a.created_at, u.name, a.action, a.details FROM admin_actions a "
                              "JOIN users u ON u.user_id = a.admin_id ORDER BY a.action_id DESC LIMIT 50", {}, actions);
    put_conn(cid);
    if (!ok || !cols_ok(logs, 8) || !cols_ok(workers, 3) || !cols_ok(actions, 4)) return error_page(client, req, 500, "Database error");

    // worker bars: sabse zyada wale ka 100%
    long long top = 0;
    for (auto& w : workers) top = max(top, atoll(w[1].c_str()));
    string worker_rows;
    for (auto& w : workers) {
        long long count = atoll(w[1].c_str());
        int pct = top > 0 ? (int)(count * 100 / top) : 0;
        char avg[32];
        snprintf(avg, sizeof avg, "%.2f", atof(w[2].c_str()));
        worker_rows += render("admin_worker_row", {{"worker", w[0]}, {"count", w[1]}, {"avg", avg}, {"pct", to_string(pct)}});
    }
    if (workers.empty()) worker_rows = render("admin_none_row", {{"message", "No requests in the last 5 minutes."}});

    string log_rows;
    for (auto& l : logs)
        log_rows += render("admin_log_row", {{"time", l[0]}, {"method", l[1]}, {"path", l[2]}, {"status", l[3]}, {"status_class", "s" + l[3].substr(0, 1)},
                                             {"latency", l[4]}, {"queue", l[5]}, {"worker", l[6]}, {"user", l[7]}});
    if (logs.empty()) log_rows = render("admin_none_row", {{"message", "No log rows match."}});

    string action_rows;
    for (auto& a : actions)
        action_rows += render("admin_activity_row", {{"time", a[0]}, {"admin", a[1]}, {"action", a[2]}, {"details", a[3]}});
    if (actions.empty()) action_rows = render("admin_none_row", {{"message", "No admin changes yet."}});

    string class_options = render("search_option", {{"value", ""}, {"label", "All status classes"}, {"selected", cls.empty() ? "selected" : ""}});
    for (const char* c : {"2xx", "3xx", "4xx", "5xx"})
        class_options += render("search_option", {{"value", c}, {"label", c}, {"selected", cls == c ? "selected" : ""}});

    string head = refresh ? render("meta_refresh", {{"seconds", "3"}}) : "";
    return send_admin(client, req, 200, "Logs", "logs",
                      render("admin_logs", {{"worker", worker}, {"path", path}, {"class_options", class_options},
                                            {"refresh_checked", refresh ? "checked" : ""}, {"worker_rows", worker_rows},
                                            {"log_rows", log_rows}, {"log_count", to_string(logs.size()) + " of the last 100 requests"},
                                            {"action_rows", action_rows}}), head);
}

// /admin ... : guard, phir path ke hisaab se page
static int admin_route(SOCKET client, const Request& req) {
    int guard = require_admin(client, req);
    if (guard != 0) return guard;

    const string& p = req.path;
    if (p == "/admin") return admin_dashboard_page(client, req);
    if (p == "/admin/products") return admin_products_page(client, req);
    if (p == "/admin/products/new") return admin_product_new_page(client, req);
    if (p == "/admin/products/edit") return admin_product_edit_page(client, req);
    if (p == "/admin/products/restock") return admin_restock_page(client, req);
    if (p == "/admin/products/toggle") return admin_toggle_page(client, req);
    if (p == "/admin/categories") return admin_categories_page(client, req);
    if (p == "/admin/categories/add") return admin_category_add_page(client, req);
    if (p == "/admin/categories/rename") return admin_category_rename_page(client, req);
    if (p == "/admin/categories/delete") return admin_category_delete_page(client, req);
    if (p == "/admin/orders") return admin_orders_page(client, req);
    if (p == "/admin/order") return admin_order_page(client, req);
    if (p == "/admin/order/status") return admin_order_status_page(client, req);
    if (p == "/admin/users") return admin_users_page(client, req);
    if (p == "/admin/users/role") return admin_user_role_page(client, req);
    if (p == "/admin/users/logout-all") return admin_user_logout_all_page(client, req);
    if (p == "/admin/sessions") return admin_sessions_page(client, req);
    if (p == "/admin/sessions/end") return admin_session_end_page(client, req);
    if (p == "/admin/logs") return admin_logs_page(client, req);
    return error_page(client, req, 404, "404 Not Found");
}

// path ke hisaab se page bhejo; status code return karta hai
int route(SOCKET client, Request& req) {
    // /metrics ko login/session ki zaroorat nahi (aur DB ki bhi nahi): load_user se pehle
    if (req.path == "/metrics") return metrics_page(client, req, false);
    if (req.path == "/metrics.json") return metrics_page(client, req, true);

    load_user(req);                     // pehle dekho kaun hai (har request par)
    const string& path = req.path;

    if (path == "/admin" || path.rfind("/admin/", 0) == 0) return admin_route(client, req);   // guard andar

    if (path == "/") return home_page(client, req);
    if (path == "/products") return products_page(client, req);
    if (path == "/product") return product_page(client, req);
    if (path == "/signup") return signup_page(client, req);
    if (path == "/login") return login_page(client, req);
    if (path == "/logout") return logout_page(client, req);
    if (path == "/account/password") return account_password_page(client, req);
    if (path == "/sessions") return sessions_page(client, req);
    if (path == "/cart") return cart_page(client, req);
    if (path == "/cart/add") return cart_add_page(client, req);
    if (path == "/cart/update") return cart_update_page(client, req);
    if (path == "/cart/remove") return cart_remove_page(client, req);
    if (path == "/addresses") return addresses_page(client, req);
    if (path == "/addresses/new") return address_new_page(client, req);
    if (path == "/addresses/edit") return address_edit_page(client, req);
    if (path == "/addresses/delete") return address_delete_page(client, req);
    if (path == "/addresses/default") return address_default_page(client, req);
    if (path == "/checkout") return checkout_page(client, req);
    if (path == "/orders") return orders_page(client, req);
    if (path == "/order") return order_page(client, req);
    if (path == "/order/cancel") return order_cancel_page(client, req);
    if (path == "/test") return test_page(client, req);

    // neeche ke do route sirf testing ke liye
    if (path == "/slow") {
        this_thread::sleep_for(chrono::seconds(3));
        send_response(client, 200, HTML, render("plain", {{"message", "done"}}), with_cookie_fix(req, ""));
        return 200;
    }
    if (path == "/dbtest") return db_test_page(client, req, "Products in database", "SELECT COUNT(*) FROM products");
    if (path == "/dbslow") return db_test_page(client, req, "Slept 2 seconds", "SELECT SLEEP(2)");

    return error_page(client, req, 404, "404 Not Found");
}

