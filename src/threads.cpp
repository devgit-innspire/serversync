// threads.cpp - client queue, worker threads, logger thread, metrics, mutex + semaphores, thread-safe log
// Owner: Shoyam20
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>
#include <mutex>
#include <semaphore>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include "common.h"
using namespace std;

// monotonic clock (system time badalne se nahi hilti): latency naapne ke liye
long long now_us() {
    return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- queue: circular buffer (array). Har entry me socket ke saath accept ka time ----
static ClientJob q[QUEUE_SIZE];
static int head = 0;    // yahan se nikalte hain
static int tail = 0;    // yahan daalte hain
static int queue_count = 0;     // queue me asli clients (stop signal ke INVALID_SOCKET nahi) ginti, /metrics ke liye

// mutex: array/head/tail/queue_count ko ek time par sirf ek thread chhede
static mutex qlock;

// items = queue me kitne clients hain (0 se start): khali hone par worker so jata hai
// slots = queue me kitni jagah khali hai (QUEUE_SIZE se start): bhar jane par accept loop ruk jata hai
static counting_semaphore<> items(0);
static counting_semaphore<> slots(QUEUE_SIZE);

// ---- workers ----
static thread workers[WORKERS];

// har thread ka apna number (0 = main, -1 = cleanup, -2 = logger, -3 = delivery); thread_local = har thread ki apni copy
static thread_local int my_id = 0;

// log mutex: warna do threads ki lines beech me mix ho jayengi
static mutex log_lock;

int thread_id() {
    return my_id;
}

void log_tag(const string& tag, const string& msg) {
    time_t now = time(nullptr);
    tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);      // thread-safe localtime (Linux me arguments ulte order me)
#endif
    char tm_buf[16];
    strftime(tm_buf, sizeof(tm_buf), "%H:%M:%S", &t);

    log_lock.lock();
    cout << "[" << tm_buf << "][" << tag << "] " << msg << endl;
    log_lock.unlock();
}

// is thread ka naam: worker = THREAD n, cleanup = CLEANUP, logger = LOGGER, delivery = DELIVERY, baaki (main) = SERVER
string thread_name() {
    if (my_id > 0) return "THREAD " + to_string(my_id);
    if (my_id == -1) return "CLEANUP";
    if (my_id == -2) return "LOGGER";
    if (my_id == -3) return "DELIVERY";
    return "SERVER";
}

// tag apne aap: thread ke naam se
void log_msg(const string& msg) {
    log_tag(thread_name(), msg);
}

// bahut baar aane wali lines ("conn taken", har request): sirf VERBOSE_LOG = 1 par. Dono tarah ke metrics/DB me
// yeh sab pehle se hai, console par bhar dene se bas asli errors dab jaate hain
void log_verbose(const string& tag, const string& msg) {
    if (VERBOSE_LOG) log_tag(tag, msg);
}

void log_msg_verbose(const string& msg) {
    if (VERBOSE_LOG) log_msg(msg);
}

// ---- metrics: sab ek mutex ke andar (kai threads badhate hain, /metrics padhta hai) ----
static mutex metrics_lock;
static time_t started_at = time(nullptr);
static long long m_total = 0;
static long long m_class[6] = {0, 0, 0, 0, 0, 0};   // m_class[2] = 2xx ... m_class[5] = 5xx
static long long m_rejected_503 = 0;
static long long m_timeouts_408 = 0;
static long long m_commit = 0, m_rollback = 0;
static long long m_dropped = 0;
static int m_busy = 0;

// pichle LATENCY_WINDOW requests ki latency (microseconds): ring buffer, purani apne aap overwrite
static long long lat_ring[LATENCY_WINDOW];
static int lat_pos = 0;
static int lat_count = 0;

// pichle RPS_SECONDS second ke requests: har second ke liye ek bucket (bucket_sec = kaun sa second, bucket_cnt = ginti)
static long long bucket_sec[RPS_SECONDS];
static long long bucket_cnt[RPS_SECONDS];

// ek jawab ginti me jodo (metrics_lock pehle se liya hona chahiye)
static void count_response(int status) {
    m_total++;
    int cls = status / 100;
    if (cls >= 2 && cls <= 5) m_class[cls]++;

    long long sec = (long long)time(nullptr);
    int i = (int)(sec % RPS_SECONDS);
    if (bucket_sec[i] != sec) {         // is bucket me purana second tha: naye second ke liye reset
        bucket_sec[i] = sec;
        bucket_cnt[i] = 0;
    }
    bucket_cnt[i]++;
}

// worker ne ek request poori ki
static void metric_request(int status, long long latency_us) {
    metrics_lock.lock();
    count_response(status);
    if (status == 408) m_timeouts_408++;
    lat_ring[lat_pos] = latency_us;
    lat_pos = (lat_pos + 1) % LATENCY_WINDOW;
    if (lat_count < LATENCY_WINDOW) lat_count++;
    metrics_lock.unlock();
}

// queue full: accept thread ne 503 bheja (latency ring me nahi: wo request ka asli kaam nahi hua)
long long metric_rejected() {
    metrics_lock.lock();
    count_response(503);
    m_rejected_503++;
    long long n = m_rejected_503;
    metrics_lock.unlock();
    return n;
}

void metric_checkout(bool commit) {
    metrics_lock.lock();
    if (commit) m_commit++; else m_rollback++;
    metrics_lock.unlock();
}

static void metric_busy(int delta) {
    metrics_lock.lock();
    m_busy += delta;
    metrics_lock.unlock();
}

static void metric_dropped(long long n) {
    metrics_lock.lock();
    m_dropped += n;
    metrics_lock.unlock();
}

Metrics metrics_snapshot() {
    Metrics m;
    vector<long long> lat;

    metrics_lock.lock();
    time_t now = time(nullptr);
    m.uptime_s = (long long)(now - started_at);
    m.total = m_total;
    m.c2xx = m_class[2]; m.c3xx = m_class[3]; m.c4xx = m_class[4]; m.c5xx = m_class[5];
    m.rejected_503 = m_rejected_503;
    m.timeouts_408 = m_timeouts_408;
    m.busy_workers = m_busy;
    m.checkout_commit = m_commit;
    m.checkout_rollback = m_rollback;
    m.dropped_logs = m_dropped;
    lat.assign(lat_ring, lat_ring + lat_count);     // copy: sorting lock ke bahar

    long long recent = 0;                           // pichle RPS_SECONDS second (abhi ka adhura second bhi)
    for (int i = 0; i < RPS_SECONDS; i++)
        if (bucket_sec[i] > (long long)now - RPS_SECONDS) recent += bucket_cnt[i];
    long long window = m.uptime_s < RPS_SECONDS ? (m.uptime_s < 1 ? 1 : m.uptime_s) : RPS_SECONDS;
    m.rps = (double)recent / (double)window;
    metrics_lock.unlock();

    // average aur p95 (sort karke 95% wali position)
    m.latency_samples = (int)lat.size();
    if (!lat.empty()) {
        sort(lat.begin(), lat.end());
        long long sum = 0;
        for (long long v : lat) sum += v;
        m.avg_ms = (double)sum / (double)lat.size() / 1000.0;
        size_t idx = (size_t)ceil(0.95 * (double)lat.size());
        m.p95_ms = (double)lat[(idx < 1 ? 1 : idx) - 1] / 1000.0;
    }

    qlock.lock();
    m.queue_length = queue_count;
    qlock.unlock();
    m.free_db_conns = db_free_connections();
    return m;
}

// ---- client queue ----

// slot mil chuka hai, ab client array me rakho (push_client aur stop_workers dono use karte hain)
static void put_client(const ClientJob& job) {
    qlock.lock();           // lock sirf chhote kaam ke liye, wait karte time nahi
    q[tail] = job;
    tail = (tail + 1) % QUEUE_SIZE;
    if (job.sock != INVALID_SOCKET) queue_count++;
    qlock.unlock();
    items.release();        // worker ko batao: ek client mil gaya
}

bool push_client(const ClientJob& job) {
    if (!slots.try_acquire()) return false;   // jagah nahi: ruko mat, caller ko batao
    put_client(job);
    return true;
}

ClientJob pop_client() {
    items.acquire();        // pehle item ka wait (queue khali ho to yahin so jao)
    qlock.lock();
    ClientJob job = q[head];
    q[head] = ClientJob();
    head = (head + 1) % QUEUE_SIZE;
    if (job.sock != INVALID_SOCKET) queue_count--;
    qlock.unlock();
    slots.release();        // accept loop ko batao: ek jagah khali hui
    return job;
}

// ---- request_logs: memory queue + ek logger thread ----
// Worker ko DB ka intezaar kabhi nahi karna chahiye, isliye woh sirf memory me entry rakhta hai (microseconds).
// Logger thread har LOG_FLUSH_SECONDS me poori queue ek saath utha kar multi-row INSERT karta hai.
static vector<LogEntry> rlog_pending;       // bounded: LOG_QUEUE_SIZE se zyada nahi
static mutex rlog_lock;                     // rlog_pending aur rlog_stop ko bachata hai
static condition_variable rlog_cv;
static bool rlog_stop = false;
static thread logger_thread;

// request_logs ke liye saaf text: sirf printable ASCII (baaki byte '?'), max length. Galat byte DB INSERT fail kara sakta hai
static string clean_for_log(const string& s, size_t max_len) {
    string out;
    for (char c : s) {
        if (out.size() >= max_len) break;
        out += (c >= 32 && c <= 126) ? c : '?';
    }
    return out;
}

void push_request_log(const LogEntry& e) {
    rlog_lock.lock();
    if (rlog_pending.size() >= (size_t)LOG_QUEUE_SIZE) {    // queue full: DB dheemi hai. Request ko rokna nahi: entry drop
        rlog_lock.unlock();
        metric_dropped(1);
        return;
    }
    rlog_pending.push_back(e);
    rlog_lock.unlock();
}

// jo entries aayi unhe LOG_BATCH_MAX ke tukdon me DB me daalo. Fail ho to wo tukda drop (gina jaata hai), request par asar nahi
static void flush_logs(const vector<LogEntry>& batch) {
    static long long last_error_us = 0;             // sirf logger thread chalata hai
    for (size_t from = 0; from < batch.size(); from += LOG_BATCH_MAX) {
        size_t to = min(batch.size(), from + (size_t)LOG_BATCH_MAX);
        vector<LogEntry> chunk(batch.begin() + from, batch.begin() + to);
        if (db_insert_request_logs(chunk) < 0) {
            metric_dropped((long long)chunk.size());
            // har second wahi error na chhape (jaise table bana hi nahi): 30 second me ek baar
            if (last_error_us == 0 || now_us() - last_error_us > 30LL * 1000000LL) {
                last_error_us = now_us();
                log_tag("LOGGER", "could not save " + to_string(chunk.size()) +
                                  " request log rows (dropped). Is the request_logs table created?");
            }
        }
    }
}

static void logger_loop() {
    my_id = -2;                             // logs me LOGGER dikhe
    db_thread_start();
    log_tag("LOGGER", "started (batch insert every " + to_string(LOG_FLUSH_SECONDS) + " s, queue " +
                      to_string(LOG_QUEUE_SIZE) + ")");

    vector<LogEntry> batch;                 // har baar wahi buffer: swap se rlog_pending ko bhi bada buffer mil jata hai
    batch.reserve(LOG_QUEUE_SIZE);

    unique_lock<mutex> lk(rlog_lock);
    while (true) {
        // sleep nahi: condition_variable ke deadline wait se stop_logger turant jaga sakta hai
        auto deadline = chrono::steady_clock::now() + chrono::seconds(LOG_FLUSH_SECONDS);
        while (!rlog_stop && rlog_cv.wait_until(lk, deadline) == cv_status::no_timeout) {
        }

        batch.clear();
        batch.swap(rlog_pending);           // poori queue ek jhatke me (O(1)); mutex jaldi chhoot jata hai
        bool stopping = rlog_stop;
        lk.unlock();

        flush_logs(batch);                  // DB ka kaam mutex ke bina: workers push karte rehte hain

        lk.lock();
        // band hona hai aur ab kuch bacha nahi (workers pehle hi band ho chuke hain) to nikal jao,
        // warna ek baar aur: jo swap ke baad aaya use bhi flush karna hai
        if (stopping && rlog_pending.empty()) break;
    }
    lk.unlock();

    db_thread_end();
    log_tag("LOGGER", "stopped (all pending logs flushed)");
}

void start_logger() {
    rlog_stop = false;
    rlog_pending.reserve(LOG_QUEUE_SIZE);
    logger_thread = thread(logger_loop);
}

void stop_logger() {
    rlog_lock.lock();
    rlog_stop = true;
    rlog_lock.unlock();
    rlog_cv.notify_all();                   // sota hua logger turant jaag kar bacha hua flush karega
    if (logger_thread.joinable()) logger_thread.join();
}

// ---- workers ----

// ek request poori hui: metrics me ginti aur request_logs ke liye entry (dono memory me, DB ka wait nahi)
static void finish_request(const RequestInfo& info, long long accept_us, long long picked_us, long long done_us) {
    metric_request(info.status, done_us - accept_us);

    LogEntry e;
    e.method = clean_for_log(info.method, 10);          // table me VARCHAR(10)
    e.path = clean_for_log(info.path, 255);             // VARCHAR(255), query string ke bina
    e.status = info.status;
    e.latency_ms = (double)(done_us - accept_us) / 1000.0;          // accept -> reply bheji gayi
    e.queue_wait_ms = (double)(picked_us - accept_us) / 1000.0;     // accept -> worker ne uthaya
    e.worker_id = my_id;
    e.user_id = info.user_id;
    e.created = (long long)time(nullptr);
    push_request_log(e);
}

// har worker thread yahi loop chalata hai
static void worker_loop(int id) {
    my_id = id;
    db_thread_start();      // MySQL library ko is thread ke baare me batao
    log_msg("worker started");
    while (true) {
        ClientJob job = pop_client();
        if (job.sock == INVALID_SOCKET) break;    // INVALID_SOCKET = "band ho jao" ka signal
        long long picked = now_us();              // queue_wait_ms yahan tak: accept se ab tak
        metric_busy(1);
        RequestInfo info = handleClient(job);
        long long done = now_us();
        metric_busy(-1);
        if (info.status > 0) finish_request(info, job.accept_us, picked, done);
    }
    db_thread_end();        // is thread ka MySQL memory free karo
    log_msg("worker stopped");
}

void start_workers() {
    for (int i = 0; i < WORKERS; i++)
        workers[i] = thread(worker_loop, i + 1);
}

void stop_workers() {
    // har worker ke liye ek "band ho jao" signal; pehle wale clients phir bhi pure honge (FIFO)
    // yahan try_acquire nahi: queue full ho to bhi workers ke khali karne tak ruk ke jagah lenge
    ClientJob stop;                         // sock = INVALID_SOCKET
    for (int i = 0; i < WORKERS; i++) {
        slots.acquire();
        put_client(stop);
    }
    for (int i = 0; i < WORKERS; i++) workers[i].join();
}

// ---- cleanup thread: expired sessions ko band karta rehta hai ----
static thread cleanup_thread;
static mutex cleanup_lock;                  // cleanup_stop ko bachata hai
static condition_variable cleanup_cv;       // "ab band ho jao" ya "time poora" par thread ko jagata hai
static bool cleanup_stop = false;

static void cleanup_loop() {
    my_id = -1;                             // logs me THREAD n nahi, CLEANUP dikhe
    db_thread_start();
    log_tag("CLEANUP", "started (every " + to_string(CLEANUP_SECONDS) + " seconds)");

    unique_lock<mutex> lk(cleanup_lock);
    while (!cleanup_stop) {
        // sleep(60) nahi: condition_variable.wait_until sote hue bhi notify se turant jaag jata hai.
        // Spurious wakeup (bina wajah jagna) ho to deadline tak dobara wait karte hain
        auto deadline = chrono::steady_clock::now() + chrono::seconds(CLEANUP_SECONDS);
        while (!cleanup_stop && cleanup_cv.wait_until(lk, deadline) == cv_status::no_timeout) {
        }
        if (cleanup_stop) break;

        lk.unlock();                        // DB ka kaam mutex ke bina (warna stop_cleanup ruk jata)
        int n = close_expired_sessions();
        if (n < 0) log_tag("CLEANUP", "failed to close expired sessions");
        else if (n > 0) log_tag("CLEANUP", "closed " + to_string(n) + " expired sessions");
        lk.lock();
    }
    lk.unlock();

    db_thread_end();
    log_tag("CLEANUP", "stopped");
}

// ---- delivery thread: time aa chuke orders ko ek step aage badhata hai (PLACED -> PACKED -> ... -> DELIVERED) ----
// Time DB me (orders.next_step_at) rehta hai, memory me nahi: isliye server restart ke baad bhi order wahin se aage badhta hai
static thread delivery_thread;
static mutex delivery_lock;                 // delivery_stop ko bachata hai
static condition_variable delivery_cv;      // "ab band ho jao" ya "tick poora" par thread ko jagata hai
static bool delivery_stop = false;

static void delivery_loop() {
    my_id = -3;                             // logs me DELIVERY dikhe
    db_thread_start();
    log_tag("DELIVERY", "started (check every " + to_string(DELIVERY_TICK_SECONDS) + " seconds)");

    unique_lock<mutex> lk(delivery_lock);
    while (!delivery_stop) {
        // cleanup thread jaisa: sleep nahi, condition_variable, taaki shutdown turant ho
        auto deadline = chrono::steady_clock::now() + chrono::seconds(DELIVERY_TICK_SECONDS);
        while (!delivery_stop && delivery_cv.wait_until(lk, deadline) == cv_status::no_timeout) {
        }
        if (delivery_stop) break;

        lk.unlock();                        // DB ka kaam mutex ke bina (warna stop_delivery ruk jata)
        if (deliver_due_orders() < 0) log_tag("DELIVERY", "failed to read due orders");
        lk.lock();
    }
    lk.unlock();

    db_thread_end();
    log_tag("DELIVERY", "stopped");
}

void start_delivery() {
    if (!AUTO_DELIVERY) {
        log_tag("DELIVERY", "off (AUTO_DELIVERY = 0): only the admin changes order status");
        return;
    }
    delivery_stop = false;
    delivery_thread = thread(delivery_loop);
}

void stop_delivery() {
    delivery_lock.lock();
    delivery_stop = true;
    delivery_lock.unlock();
    delivery_cv.notify_all();               // sota hua thread turant jaag jayega
    if (delivery_thread.joinable()) delivery_thread.join();
}

// main.cpp ko chhue bina: delivery thread bhi cleanup ke saath shuru/band hota hai (dono background kaam DB use karte hain)
void start_cleanup() {
    cleanup_stop = false;
    cleanup_thread = thread(cleanup_loop);
    start_delivery();
}

void stop_cleanup() {
    stop_delivery();
    cleanup_lock.lock();
    cleanup_stop = true;
    cleanup_lock.unlock();
    cleanup_cv.notify_all();                // sota hua thread turant jaag jayega
    if (cleanup_thread.joinable()) cleanup_thread.join();
}
