// orders_check.cpp - runs the REAL order_change() from src/db.cpp against the throwaway test MySQL
// (127.0.0.1:3399, root, no password). src/db.cpp is #included so the pool arrays are reachable: we fill the pool
// with our own connections, so init_db() and db.conf are never used.
// usage: orders_check <job> [<job> ...]   job = order_id:to:user_id:admin(0/1)[:expected_from]
//        (one thread per job, all started together)
#include "../src/db.cpp"
#include <iostream>
#include <sstream>
#include <thread>

// ---- stubs for what db.cpp expects from other files ----
static mutex out_lock;
void log_tag(const string& tag, const string& msg) {
    lock_guard<mutex> g(out_lock);
    cout << "[" << tag << "] " << msg << endl;
}
int thread_id() { return 0; }
string thread_name() { return "TEST"; }
string trim(const string& s) { return s; }
void log_verbose(const string& tag, const string& msg) { log_tag(tag, msg); }
void metric_checkout(bool) {}

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

    int n = argc - 1;
    vector<OrderResult> res(n);
    vector<thread> threads;
    for (int i = 0; i < n; i++) {
        vector<string> f;
        stringstream ss(argv[i + 1]);
        string part;
        while (getline(ss, part, ':')) f.push_back(part);
        while (f.size() < 5) f.push_back("");
        threads.emplace_back([&res, i, f] {
            mysql_thread_init();
            res[i] = order_change(atoi(f[0].c_str()), f[1], atoi(f[2].c_str()), f[3] == "1", f[4]);
            mysql_thread_end();
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 0; i < n; i++)
        cout << "RESULT job=" << i << " status=" << res[i].status << " from=" << res[i].from << " msg=" << res[i].message << endl;

    for (int i = 0; i < DB_POOL; i++) {
        mysql_query(pool[i], "SELECT @@autocommit");
        MYSQL_RES* r = mysql_store_result(pool[i]);
        MYSQL_ROW row = mysql_fetch_row(r);
        cout << "AUTOCOMMIT conn=" << (i + 1) << " value=" << row[0] << endl;
        mysql_free_result(r);
    }
    return 0;
}
