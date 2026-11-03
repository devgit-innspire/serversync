// password_check.cpp - runs the REAL password_change() from src/db.cpp against the throwaway test MySQL
// (127.0.0.1:3399, root, no password). src/db.cpp is #included so the pool arrays are reachable: we fill the pool
// with our own connections, so init_db() and db.conf are never used.
// Needs (tests/test_password.py makes them): user 1 (password Old@pass1) with sessions AAAA.. (current), BBBB.., CCCC..,
// user 2 with session DDDD... Prints "KEY value" lines.
#include "../src/db.cpp"
#include <iostream>

// ---- stubs for what db.cpp expects from other files ----
static mutex out_lock;
void log_tag(const string& tag, const string& msg) {
    lock_guard<mutex> g(out_lock);
    cerr << "[" << tag << "] " << msg << endl;
}
int thread_id() { return 0; }
string thread_name() { return "TEST"; }
string trim(const string& s) { return s; }
void log_verbose(const string& tag, const string& msg) { log_tag(tag, msg); }
void metric_checkout(bool) {}

static void show(const string& key, const PasswordResult& r) {
    cout << key << " " << r.status << " ended=" << r.ended << " " << r.message << endl;
}

int main(int argc, char** argv) {
    mysql_library_init(0, nullptr, nullptr);
    for (int i = 0; i < DB_POOL; i++) {
        MYSQL* m = mysql_init(nullptr);
        if (!mysql_real_connect(m, "127.0.0.1", "root", "", "ecommerce_db", 3399, nullptr, CLIENT_FOUND_ROWS)) {
            cerr << "connect failed: " << mysql_error(m) << endl;
            return 2;
        }
        pool[i] = m;
        free_ids[free_count++] = i + 1;
    }
    free_sem.release(DB_POOL);

    // usage: password_check fail | ok   (python looks at the database between the two runs)
    const string mode = argc > 1 ? argv[1] : "";
    const string keep(64, 'a');
    if (mode == "fail") {
        show("WRONG", password_change(1, "wrong", "New@pass9", keep));
        show("WRONG_CASE", password_change(1, "old@pass1", "New@pass9", keep));     // BINARY: capital / small letters matter
        show("NO_USER", password_change(99, "Old@pass1", "New@pass9", keep));
        show("OTHER_USERS_PASSWORD", password_change(1, "Other@pass1", "New@pass9", keep));
    } else {
        show("OK", password_change(1, "Old@pass1", "New@pass9", keep));
        show("OLD_NOW_WRONG", password_change(1, "Old@pass1", "Third@pass9", keep));
    }
    return 0;
}
