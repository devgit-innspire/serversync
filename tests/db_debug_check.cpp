// db_debug_check.cpp - used by test_db_debug.py: feeds sample statements to debug_log_sql() from src/db.cpp
// (it is #included so the static function is reachable) and prints what would be logged. No database needed.
#include "../src/db.cpp"
#include <iostream>

void log_tag(const string& tag, const string& msg) { cout << "[" << tag << "] " << msg << endl; }
int thread_id() { return 0; }
string thread_name() { return "TEST"; }
string trim(const string& s) { return s; }
void log_verbose(const string&, const string&) {}
void metric_checkout(bool) {}

static void show(const string& title, const string& sql, const vector<string>& args) {
    cout << "== " << title << endl;
    vector<unsigned long> lens;
    for (auto& a : args) lens.push_back((unsigned long)a.size());
    debug_log_sql(sql, args, lens.data());
}

int main() {
    show("login", "SELECT user_id FROM users WHERE email = ? AND BINARY password = ?", {"secret@x.com", "hunter22"});
    show("signup", "INSERT INTO users (name, email, password) VALUES (?, ?, ?)", {"Bobby Tables", "bob@x.com", "p4ssw0rd"});
    show("session lookup", "SELECT s.user_id, u.name FROM user_sessions s JOIN users u ON u.user_id = s.user_id "
                           "WHERE s.session_id = ? AND s.is_active = 1", {string(64, 'a')});
    show("session insert", "INSERT INTO user_sessions (session_id, user_id, expires_at) VALUES (?, ?, NOW())",
         {string(64, 'b'), "3"});
    show("orders", "INSERT INTO orders (user_id, cart_id, total, status) SELECT ?, ?, SUM(p.price), 'PLACED' FROM x WHERE c = ?",
         {"4", "7", "7"});
    show("search", "SELECT name FROM products WHERE name LIKE ? ESCAPE '!'", {"%lap%"});
    show("long value", "SELECT 1 WHERE a = ?", {string(500, 'x')});
    return 0;
}
