// threads_check.cpp - tests the queue timing, metrics, async request logger and log gating of src/threads.cpp.
// threads.cpp is #included (so its static functions are reachable); the database and the request handler are
// replaced by stubs below, so no MySQL, no server, no db.conf is needed.
// usage: threads_check <scenario>        (see test_threads.py for what each scenario expects)
#include "../src/threads.cpp"
#include <cstring>
#include <iostream>

// ---- stubs ----
static mutex cap_lock;
static vector<vector<LogEntry>> chunks;         // every batch the "database" received
static int insert_delay_ms = 0;                 // makes the fake INSERT slow
static bool insert_fail = false;                // makes the fake INSERT fail

int db_insert_request_logs(const vector<LogEntry>& rows) {
    if (insert_delay_ms > 0) this_thread::sleep_for(chrono::milliseconds(insert_delay_ms));
    if (insert_fail) return -1;
    lock_guard<mutex> g(cap_lock);
    chunks.push_back(rows);
    return (int)rows.size();
}
int db_free_connections() { return 3; }
void db_thread_start() {}
void db_thread_end() {}
int close_expired_sessions() { return 0; }

// the request handler: job.ip = "<status>:<sleep ms>" tells it what to do
RequestInfo handleClient(const ClientJob& job) {
    int status = atoi(job.ip.c_str());
    size_t colon = job.ip.find(':');
    int ms = colon == string::npos ? 0 : atoi(job.ip.c_str() + colon + 1);
    if (ms > 0) this_thread::sleep_for(chrono::milliseconds(ms));
    RequestInfo i;
    i.method = "GET";
    i.path = "/x";
    i.status = status;
    i.user_id = (status == 200) ? 7 : 0;
    return i;
}

static void push(const string& behavior) {
    ClientJob j;
    j.sock = (SOCKET)1000;                      // fake socket: the stub handler never touches it
    j.ip = behavior;
    while (true) {
        j.accept_us = now_us();                 // "accept" time = the moment of the push attempt (like runServer)
        if (push_client(j)) break;
        this_thread::sleep_for(chrono::milliseconds(1));    // queue full: wait a bit, stamp again
    }
}

static void wait_total(long long n) {
    for (int i = 0; i < 30000; i++) {
        if (metrics_snapshot().total >= n) return;
        this_thread::sleep_for(chrono::milliseconds(1));
    }
}

static void print_metrics(const Metrics& m) {
    cout << "TOTAL " << m.total << "\nC2XX " << m.c2xx << "\nC3XX " << m.c3xx << "\nC4XX " << m.c4xx << "\nC5XX " << m.c5xx
         << "\nREJECTED " << m.rejected_503 << "\nTIMEOUTS " << m.timeouts_408 << "\nBUSY " << m.busy_workers
         << "\nQUEUE " << m.queue_length << "\nFREE_DB " << m.free_db_conns << "\nSAMPLES " << m.latency_samples
         << "\nAVG_MS " << m.avg_ms << "\nP95_MS " << m.p95_ms << "\nRPS " << m.rps << "\nCOMMIT " << m.checkout_commit
         << "\nROLLBACK " << m.checkout_rollback << "\nDROPPED " << m.dropped_logs << endl;
}

static size_t inserted_rows() {
    lock_guard<mutex> g(cap_lock);
    size_t n = 0;
    for (auto& c : chunks) n += c.size();
    return n;
}

int main(int argc, char** argv) {
    string s = argc > 1 ? argv[1] : "";

    if (s == "metrics") {
        start_logger(); start_workers();
        // 4 workers x 10 ms of work = one request per 2.5 ms of capacity: pushing one every 4 ms keeps the queue empty,
        // so each request's latency is just its ~10 ms of work (queue wait is part of latency, tested in "timing")
        for (int i = 0; i < 100; i++) { push("200:10"); this_thread::sleep_for(chrono::milliseconds(4)); }
        for (int i = 0; i < 20; i++) push("302:0");
        for (int i = 0; i < 7; i++) push("404:0");
        for (int i = 0; i < 3; i++) push("500:0");
        push("408:0");
        wait_total(131);
        this_thread::sleep_for(chrono::milliseconds(50));
        metric_rejected(); metric_rejected();
        for (int i = 0; i < 6; i++) metric_checkout(true);
        metric_checkout(false);
        print_metrics(metrics_snapshot());
        stop_workers(); stop_cleanup(); stop_logger();
        cout << "INSERTED " << inserted_rows() << endl;
    }
    else if (s == "window") {           // build with LATENCY_WINDOW = 50: only the LAST 50 requests count
        start_logger(); start_workers();
        for (int i = 0; i < 30; i++) push("200:50");        // slow ones first ...
        wait_total(30);
        for (int i = 0; i < 50; i++) push("200:0");         // ... then 50 fast ones push them out of the window
        wait_total(80);
        this_thread::sleep_for(chrono::milliseconds(30));
        print_metrics(metrics_snapshot());
        stop_workers(); stop_logger();
    }
    else if (s == "timing") {           // 8 slow requests, 4 workers: the second four must WAIT in the queue
        start_logger(); start_workers();
        for (int i = 0; i < 8; i++) push("200:200");
        wait_total(8);
        stop_workers(); stop_logger();
        for (auto& c : chunks)
            for (auto& e : c)
                cout << "ENTRY worker=" << e.worker_id << " status=" << e.status << " latency=" << e.latency_ms
                     << " wait=" << e.queue_wait_ms << " method=" << e.method << " path=" << e.path
                     << " user=" << e.user_id << " created_ago=" << (long long)time(nullptr) - e.created << endl;
    }
    else if (s == "drop") {             // build with LOG_QUEUE_SIZE = 20; the fake INSERT takes 800 ms
        insert_delay_ms = 800;
        start_logger();
        LogEntry e; e.method = "GET"; e.path = "/p"; e.status = 200;
        long long t0 = now_us();
        for (int i = 0; i < 100; i++) push_request_log(e);  // 100 pushed at once: 20 fit, 80 are dropped
        long long push_us = now_us() - t0;
        cout << "PUSH_US_100 " << push_us << "\nDROPPED_AFTER_100 " << metrics_snapshot().dropped_logs << endl;

        this_thread::sleep_for(chrono::milliseconds(1300));  // the logger is now inside the slow 800 ms INSERT
        t0 = now_us();
        for (int i = 0; i < 10; i++) push_request_log(e);   // must NOT wait for that INSERT
        cout << "PUSH_US_WHILE_INSERTING " << now_us() - t0 << endl;

        stop_logger();                                       // flushes everything that is left
        cout << "INSERTED " << inserted_rows() << "\nDROPPED " << metrics_snapshot().dropped_logs << endl;
    }
    else if (s == "batch") {            // 450 entries, LOG_BATCH_MAX = 200: chunks 200, 200, 50; stop must not wait 1 s
        start_logger();
        LogEntry e; e.method = "GET"; e.path = "/p"; e.status = 200; e.created = (long long)time(nullptr);
        for (int i = 0; i < 450; i++) push_request_log(e);
        long long t0 = now_us();
        stop_logger();
        cout << "STOP_MS " << (now_us() - t0) / 1000 << endl;
        for (auto& c : chunks) cout << "CHUNK " << c.size() << endl;
        cout << "INSERTED " << inserted_rows() << endl;
    }
    else if (s == "fail") {             // the INSERT always fails (e.g. table missing): count as dropped, stay alive
        insert_fail = true;
        start_logger();
        LogEntry e; e.method = "GET"; e.path = "/p"; e.status = 200;
        for (int i = 0; i < 30; i++) push_request_log(e);
        this_thread::sleep_for(chrono::milliseconds(1300));
        for (int i = 0; i < 5; i++) push_request_log(e);     // the logger must still be running
        stop_logger();
        cout << "DROPPED " << metrics_snapshot().dropped_logs << "\nINSERTED " << inserted_rows() << endl;
    }
    else if (s == "periodic") {         // logs are written every LOG_FLUSH_SECONDS without any stop
        start_logger();
        LogEntry e; e.method = "GET"; e.path = "/p"; e.status = 200;
        for (int i = 0; i < 5; i++) push_request_log(e);
        this_thread::sleep_for(chrono::milliseconds(1500));
        cout << "INSERTED_BEFORE_STOP " << inserted_rows() << endl;
        stop_logger();
        cout << "INSERTED " << inserted_rows() << endl;
    }
    else if (s == "stress") {           // build with LOG_QUEUE_SIZE = 20000: 8 threads x 1000 finished requests at once
        start_logger();
        vector<thread> ts;
        for (int t = 0; t < 8; t++)
            ts.emplace_back([] {
                for (int i = 0; i < 1000; i++) {
                    RequestInfo info;
                    info.method = "GET"; info.path = "/s"; info.status = (i % 4 == 0) ? 404 : 200;
                    long long a = now_us();
                    finish_request(info, a, a + 10, a + 100);
                }
            });
        for (auto& t : ts) t.join();
        stop_logger();
        print_metrics(metrics_snapshot());
        cout << "INSERTED " << inserted_rows() << endl;
    }
    else if (s == "verbose") {          // which lines reach the console?
        log_verbose("DB", "conn 1 taken by THREAD 1");
        log_msg_verbose("GET / -> 200");
        log_tag("CHECKOUT", "user 4 order 7 total 4497.00 COMMIT");
        log_tag("CLEANUP", "closed 2 expired sessions");
        log_tag("LOGGER", "started");
        log_msg("startup line");
    }
    else if (s == "sanitize") {         // what goes into request_logs: printable ASCII, max lengths
        cout << "M[" << clean_for_log("GET", 10) << "]" << endl;
        cout << "M[" << clean_for_log("ABCDEFGHIJKLMNOP", 10) << "]" << endl;
        cout << "P[" << clean_for_log(string("/caf\xC3\xA9\x01x", 8), 255) << "]" << endl;
        cout << "LEN " << clean_for_log(string(1000, 'a'), 255).size() << endl;
    }
    else {
        cerr << "unknown scenario" << endl;
        return 2;
    }
    return 0;
}
