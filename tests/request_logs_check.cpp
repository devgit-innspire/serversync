// request_logs_check.cpp - runs the REAL db_insert_request_logs() from src/db.cpp against the throwaway test MySQL
// (127.0.0.1:3399, root, no password). src/db.cpp is #included so the pool arrays are reachable and we fill the pool
// with our own connections: init_db() and db.conf are never used.
// usage: request_logs_check <rows> [bad]     ("bad" makes the LAST row have status 99, which the CHECK rejects)
#include "../src/db.cpp"
#include <iostream>

void log_tag(const string& tag, const string& msg) { cout << "[" << tag << "] " << msg << endl; }
int thread_id() { return 0; }
string thread_name() { return "TEST"; }
string trim(const string& s) { return s; }
void log_verbose(const string&, const string&) {}
void metric_checkout(bool) {}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    bool bad = argc > 2 && string(argv[2]) == "bad";

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

    vector<LogEntry> rows;
    for (int i = 0; i < n; i++) {
        LogEntry e;
        e.method = (i % 2 == 0) ? "GET" : "POST";
        e.path = "/p" + to_string(i);
        e.status = (i % 5 == 0) ? 404 : 200;
        e.latency_ms = i * 0.5 + 0.123456;          // 3 decimals are kept (DECIMAL(10,3))
        e.queue_wait_ms = 0.25;
        e.worker_id = 1 + i % 4;
        e.user_id = (i % 3 == 0) ? 0 : 5;           // 0 must become NULL
        e.created = 1760000000LL + i;               // the REAL request time, not the insert time
        rows.push_back(e);
    }
    if (n >= 2) rows[1].path = "/x'); DROP TABLE users;-- %s \\ \"";   // quotes etc. are just data (bound params)
    if (bad && !rows.empty()) rows.back().status = 99;                 // violates CHECK (status BETWEEN 100 AND 599)

    int r = db_insert_request_logs(rows);
    cout << "RESULT " << r << endl;
    return 0;
}
