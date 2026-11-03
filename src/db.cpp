// db.cpp - MySQL connection pool: db.conf padhna, get_conn/put_conn, simple query helper
// Owner: Shoyam20
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql.h>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <semaphore>
#include <thread>
#include "common.h"
using namespace std;

// ---- login details (db.conf se aate hain, code me nahi likhte) ----
static string db_host, db_user, db_pass, db_name;
static unsigned int db_port = 3306;

// ---- pool ----
static MYSQL* pool[DB_POOL];        // connections; connection number = index + 1
static int free_ids[DB_POOL];       // free list: abhi kaun se connection number khali hain
static int free_count = 0;

// mutex: free list ko ek time par sirf ek thread chhede
static mutex pool_lock;

// free_sem = kitne connections khali hain; 0 par get_conn() ruk jata hai (init_db baad me DB_POOL release karta hai)
static counting_semaphore<> free_sem(0);

// "THREAD 3", "CLEANUP" ya "SERVER" - logs ke liye
static string who() {
    return thread_name();
}

// Render jaise host par db.conf nahi hota: DB_HOST, DB_USER, DB_PASSWORD, DB_NAME (aur DB_PORT) env vars se lo
static bool load_env() {
    const char* host = getenv("DB_HOST");
    const char* user = getenv("DB_USER");
    const char* pass = getenv("DB_PASSWORD");
    const char* name = getenv("DB_NAME");
    const char* port = getenv("DB_PORT");
    if (host == nullptr || user == nullptr || pass == nullptr || name == nullptr) return false;
    db_host = host;
    db_user = user;
    db_pass = pass;
    db_name = name;
    if (port != nullptr) db_port = (unsigned int)atoi(port);
    log_tag("DB", "using DB_* environment variables");
    return true;
}

static MYSQL* open_conn();

// db.conf padho: har line "key=value"; # se shuru hone wali line comment hai
static bool load_conf() {
    if (load_env()) return true;
    ifstream f("db.conf");
    if (!f) {
        log_tag("DB", "db.conf not found. Copy db.conf.example to db.conf and fill in your MySQL details.");
        return false;
    }

    string line;
    bool has_host = false, has_user = false, has_pass = false, has_name = false;
    while (getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = trim(line.substr(0, eq));
        string val = trim(line.substr(eq + 1));

        if (key == "host") { db_host = val; has_host = true; }
        else if (key == "user") { db_user = val; has_user = true; }
        else if (key == "password") { db_pass = val; has_pass = true; }
        else if (key == "database") { db_name = val; has_name = true; }
        else if (key == "port") db_port = (unsigned int)atoi(val.c_str());
    }

    if (!has_host || !has_user || !has_pass || !has_name) {
        log_tag("DB", "db.conf must have host, user, password and database lines.");
        return false;
    }
    return true;
}

bool init_db() {
    if (!load_conf()) return false;

    // library ko ek hi baar, kisi bhi thread ke connect karne se pehle, initialise karo
    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        log_tag("DB", "mysql_library_init failed");
        return false;
    }

    for (int i = 0; i < DB_POOL; i++) {
        MYSQL* m = open_conn();
        if (m == nullptr) {
            close_db();
            return false;
        }
        pool[i] = m;
        free_ids[free_count++] = i + 1;
    }

    free_sem.release(DB_POOL);      // ab saare connections lene ke liye taiyaar hain
    log_tag("DB", "pool ready: " + to_string(DB_POOL) + " connections to " + db_name + " on " + db_host);
    return true;
}

// ek naya MySQL connection kholo; fail ho to nullptr (wajah log me)
static MYSQL* open_conn() {
    MYSQL* m = mysql_init(nullptr);
    if (m == nullptr) {
        log_tag("DB", "mysql_init failed (out of memory)");
        return nullptr;
    }
    // CLIENT_FOUND_ROWS (connect ke aakhri argument me): UPDATE ka "affected rows" = WHERE se match hui rows.
    // Iske bina MySQL sirf wo rows ginta hai jinki value sach me badli; same value likhne par 0 aata,
    // aur 0 se pata nahi chalta ki session mila hi nahi ya bas value wahi thi
    unsigned int timeout = 5;                            // server band ho to 5 sec me haar maano
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(m, db_host.c_str(), db_user.c_str(), db_pass.c_str(),
                           db_name.c_str(), db_port, nullptr, CLIENT_FOUND_ROWS) == nullptr) {
        log_tag("DB", "cannot connect to MySQL at " + db_host + " as '" + db_user +
                      "' (database " + db_name + "): " + mysql_error(m));
        mysql_close(m);
        return nullptr;
    }
    return m;
}

void close_db() {
    for (int i = 0; i < DB_POOL; i++) {
        if (pool[i] != nullptr) {
            mysql_close(pool[i]);
            pool[i] = nullptr;
        }
    }
    free_count = 0;
    mysql_library_end();
}

MYSQL* get_conn(int& id) {
    // ek connection ek time par sirf ek thread use kare: free na ho to yahin ruko
    free_sem.acquire();
    pool_lock.lock();
    if (free_count <= 0) {          // semaphore ke hisaab se aisa nahi hona chahiye; bug ho to crash nahi
        pool_lock.unlock();
        log_tag("DB", "BUG: no free connection in list");
        id = 0;
        return nullptr;
    }
    id = free_ids[--free_count];
    MYSQL* m = pool[id - 1];
    pool_lock.unlock();

    // hosted MySQL (Aiven) idle connections band kar deta hai. Ping fail = server ne connection chhod diya: naya kholo.
    // Lock ke bahar: yeh slot (id) ab sirf isi thread ka hai, koi aur pool[id - 1] nahi chhuega
    if (mysql_ping(m) != 0) {
        log_tag("DB", "conn " + to_string(id) + " lost (" + mysql_error(m) + "), reconnecting");
        MYSQL* fresh = open_conn();
        if (fresh != nullptr) {
            mysql_close(m);
            m = fresh;
            pool[id - 1] = m;
        }
    }
    log_verbose("DB", "conn " + to_string(id) + " taken by " + who());
    return m;
}

void put_conn(int id) {
    // galat id ya double put se free list array ke bahar na likha jaye
    if (id < 1 || id > DB_POOL) return;

    // log pehle: connection free list me jaate hi koi aur thread le sakta hai, uska "taken" pehle na dikhe
    log_verbose("DB", "conn " + to_string(id) + " returned");
    pool_lock.lock();
    if (free_count >= DB_POOL) {
        pool_lock.unlock();
        log_tag("DB", "BUG: put_conn called too many times");
        return;
    }
    free_ids[free_count++] = id;
    pool_lock.unlock();
    free_sem.release();             // kisi ruke hue thread ko jagao
}

// pool me abhi kitni connections khali hain (/metrics ke liye)
int db_free_connections() {
    pool_lock.lock();
    int n = free_count;
    pool_lock.unlock();
    return n;
}

// ---- prepared statements ----
// Prepared statement: pehle SQL (? ke saath) server ko jaati hai, data baad me alag.
// Isliye user ka data kabhi SQL code nahi ban sakta (SQL injection nahi hota).

const int MAX_ARGS = 4000;          // ek query me max ? ki ginti (request_logs ka multi-row INSERT: 200 rows x 8 = 1600)
const int MAX_COLS = 16;            // ek query me max columns
const unsigned long MAX_CELL = 65535;   // ek value ka max size

// result ki saari rows padho (stmt execute ho chuka hai)
// err: fail hone par MySQL error number (SELECT ... FOR UPDATE ka lock wait timeout 1205 / deadlock 1213 yahin aata hai)
static bool fetch_all(MYSQL_STMT* stmt, MYSQL_RES* meta, vector<vector<string>>& rows, int& err) {
    int cols = (int)mysql_num_fields(meta);
    if (cols > MAX_COLS) {
        log_tag("DB", "too many columns in result");
        return false;
    }
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);

    // har column ke liye ek buffer; saari values text (STRING) me maangte hain
    vector<vector<char>> bufs(cols);
    MYSQL_BIND bind[MAX_COLS];
    unsigned long lens[MAX_COLS];
    bool is_null[MAX_COLS];
    bool is_err[MAX_COLS];
    memset(bind, 0, sizeof(bind));
    for (int i = 0; i < cols; i++) {
        unsigned long size = fields[i].length < MAX_CELL ? fields[i].length : MAX_CELL;
        bufs[i].resize(size + 1);
        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].buffer = bufs[i].data();
        bind[i].buffer_length = size;
        bind[i].length = &lens[i];
        bind[i].is_null = &is_null[i];
        bind[i].error = &is_err[i];
    }

    if (mysql_stmt_bind_result(stmt, bind) != 0 || mysql_stmt_store_result(stmt) != 0) {
        err = (int)mysql_stmt_errno(stmt);
        log_tag("DB", "result failed: " + string(mysql_stmt_error(stmt)));
        return false;
    }

    while (true) {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;             // rows khatam
        if (rc != 0) {                              // error ya value buffer me nahi aayi
            err = (int)mysql_stmt_errno(stmt);
            log_tag("DB", "fetch failed: " + string(mysql_stmt_error(stmt)));
            return false;
        }
        vector<string> row;
        for (int i = 0; i < cols; i++) {
            // lens[i] buffer se bada nahi hona chahiye (truncation upar fail ho chuka), phir bhi check
            if (is_null[i] || lens[i] > bufs[i].size()) row.push_back("");
            else row.push_back(string(bufs[i].data(), lens[i]));
        }
        rows.push_back(row);
    }
    return true;
}

// result set hai to fetch_all chalao, meta ko hamesha free karo
static bool read_rows(MYSQL_STMT* stmt, vector<vector<string>>& rows, int& err) {
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (meta == nullptr) return true;               // INSERT/UPDATE jaisi query, rows nahi
    bool ok = fetch_all(stmt, meta, rows, err);
    mysql_free_result(meta);
    return ok;
}

// DEBUG_SQL (common.h) = 1: har query se pehle SQL aur har bound value (aur uski length) log karo.
// Dhyan do: 'PLACED' jaisi cheez SQL text me likhi hui hai, ? parameter nahi, isliye woh param list me nahi aati.
// Secret wali queries ki values KABHI nahi chhapti, sirf '***' (length phir bhi dikhti hai):
//   - jis SQL me "password" ho (login / signup): password, email, naam sab masked
//   - jis SQL me "user_sessions" ho: session id masked (cookie chori hone jaisa hai)
static void debug_log_sql(const string& sql, const vector<string>& args, const unsigned long* bound_len) {
    if (!DEBUG_SQL) return;
    if (sql.compare(0, 25, "INSERT INTO request_logs ") == 0) return;   // har second 1600 params: sirf shor
    string lower = sql;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    bool secret = lower.find("password") != string::npos || lower.find("user_sessions") != string::npos;

    log_tag("DB", "SQL: " + sql);
    for (size_t i = 0; i < args.size(); i++) {
        string shown = secret ? "***" : args[i].substr(0, 200);     // lambi value 200 chars par kaat do
        log_tag("DB", "param " + to_string(i + 1) + " = '" + shown + "' (len " + to_string(args[i].size()) +
                      ", bound length " + to_string(bound_len[i]) + ")");
    }
}

// CHECK constraint (error 3819) toot'ne par: DB me us constraint ki ASLI definition log karo. Isse turant dikhta hai ki
// code aur database ki list alag hai (jaise ALTER us database par chala hi nahi jisse server connected hai)
static void log_check_definition(MYSQL_STMT* stmt) {
    string msg = mysql_stmt_error(stmt);                // "Check constraint 'chk_orders_status' is violated."
    size_t a = msg.find('\'');
    size_t b = (a == string::npos) ? a : msg.find('\'', a + 1);
    if (b == string::npos) return;

    vector<vector<string>> rows;
    if (db_select_on(stmt->mysql, "SELECT DATABASE(), CHECK_CLAUSE FROM information_schema.CHECK_CONSTRAINTS "
                                  "WHERE CONSTRAINT_SCHEMA = DATABASE() AND CONSTRAINT_NAME = ?",
                     {msg.substr(a + 1, b - a - 1)}, rows) && !rows.empty() && rows[0].size() >= 2)
        log_tag("DB", "in database '" + rows[0][0] + "' the rule " + msg.substr(a, b - a + 1) + " is: " + rows[0][1]);
}

// prepare, args bind, execute. select = true: rows padho; select = false: affected rows ginti
// fail hone par err me MySQL error number rakhta hai (jaise 1062 = duplicate entry)
static bool stmt_steps(MYSQL_STMT* stmt, const string& sql, const vector<string>& args, bool select,
                       vector<vector<string>>& rows, long long& affected, int& err) {
    if (mysql_stmt_prepare(stmt, sql.c_str(), (unsigned long)sql.size()) != 0) {
        err = (int)mysql_stmt_errno(stmt);
        log_tag("DB", "prepare failed: " + string(mysql_stmt_error(stmt)));
        return false;
    }
    if ((int)args.size() > MAX_ARGS || mysql_stmt_param_count(stmt) != args.size()) {
        log_tag("DB", "wrong number of ? for this query");
        return false;
    }

    // SELECT me result set hota hai, INSERT/UPDATE/DELETE me nahi. Galat helper ho to query chalate hi nahi
    // (jaise db_select se UPDATE nahi chal sakta)
    bool has_result = mysql_stmt_field_count(stmt) > 0;
    if (has_result != select) {
        log_tag("DB", select ? "db_select used for a statement that returns no rows"
                             : "db_exec used for a statement that returns rows");
        return false;
    }

    // saare args text (STRING) ki tarah bhejte hain; MySQL zaroorat par number me badal leta hai.
    // IMPORTANT: bind aur lens ko mysql_stmt_execute() ke KHATAM hone tak zinda rehna hai. mysql_stmt_bind_param sirf
    // pointer yaad rakhta hai, asli values execute par padhta hai. Isliye yeh `if` ke bahar hain (andar rakhne par
    // execute ke waqt memory free ho chuki hoti: undefined behaviour)
    vector<MYSQL_BIND> bind(args.size());       // vector: itne saare ? stack par rakhna theek nahi (~100 bytes har ek)
    vector<unsigned long> lens(args.size());
    if (!args.empty()) {
        memset(bind.data(), 0, bind.size() * sizeof(MYSQL_BIND));
        for (size_t i = 0; i < args.size(); i++) {
            lens[i] = (unsigned long)args[i].size();
            bind[i].buffer_type = MYSQL_TYPE_STRING;
            bind[i].buffer = (void*)args[i].c_str();
            bind[i].buffer_length = lens[i];
            bind[i].length = &lens[i];
        }
        if (mysql_stmt_bind_param(stmt, bind.data()) != 0) {
            log_tag("DB", "bind failed: " + string(mysql_stmt_error(stmt)));
            return false;
        }
        debug_log_sql(sql, args, lens.data());
    } else {
        debug_log_sql(sql, args, nullptr);
    }

    if (mysql_stmt_execute(stmt) != 0) {
        err = (int)mysql_stmt_errno(stmt);
        // 1062 (duplicate email jaisa) normal baat hai, error nahi: caller sambhalega
        if (err != 1062) log_tag("DB", "execute failed: " + string(mysql_stmt_error(stmt)));
        if (err == 3819) log_check_definition(stmt);    // CHECK constraint toota: DB ki asli list dikhao
        return false;
    }
    if (select) return read_rows(stmt, rows, err);

    // affected rows = WHERE se kitni rows match hui (CLIENT_FOUND_ROWS ki wajah se, init_db dekho)
    unsigned long long n = mysql_stmt_affected_rows(stmt);
    if (n == (unsigned long long)-1) {
        log_tag("DB", "affected rows unknown");
        return false;
    }
    affected = (long long)n;
    return true;
}

// statement banao, steps chalao, statement hamesha band karo
static bool run_stmt(MYSQL* m, const string& sql, const vector<string>& args, bool select,
                     vector<vector<string>>& rows, long long& affected, int& err) {
    MYSQL_STMT* stmt = mysql_stmt_init(m);
    if (stmt == nullptr) {
        log_tag("DB", "mysql_stmt_init failed");
        return false;
    }
    bool ok = stmt_steps(stmt, sql, args, select, rows, affected, err);
    mysql_stmt_close(stmt);
    return ok;
}

// ---- *_on: pehle se li hui connection par query (caller ne get_conn kiya hai, wahi put_conn karega) ----

bool db_select_on(MYSQL* m, const string& sql, const vector<string>& args, vector<vector<string>>& rows,
                  int* err_code) {
    rows.clear();
    int err = 0;
    long long affected = 0;
    bool ok = run_stmt(m, sql, args, true, rows, affected, err);
    if (err_code != nullptr) *err_code = err;
    return ok;
}

int db_exec_on(MYSQL* m, const string& sql, const vector<string>& args, int* err_code) {
    vector<vector<string>> rows;        // exec me rows nahi aati, bas helper ke liye chahiye
    int err = 0;
    long long affected = 0;
    bool ok = run_stmt(m, sql, args, false, rows, affected, err);
    if (err_code != nullptr) *err_code = err;
    return ok ? (int)affected : -1;
}

// ---- ek query = ek connection: get_conn, query, put_conn ----

bool db_select(const string& sql, const vector<string>& args, vector<vector<string>>& rows, int* conn_id) {
    rows.clear();
    int conn = 0;
    MYSQL* m = get_conn(conn);
    if (conn_id != nullptr) *conn_id = conn;
    if (m == nullptr) return false;     // get_conn fail hua (bug), wapas karne ko kuch nahi
    bool ok = db_select_on(m, sql, args, rows);
    put_conn(conn);                 // error ho ya na ho, connection hamesha wapas
    return ok;
}

int db_exec(const string& sql, const vector<string>& args, int* err_code) {
    int conn = 0;
    MYSQL* m = get_conn(conn);
    if (m == nullptr) return -1;
    int n = db_exec_on(m, sql, args, err_code);
    put_conn(conn);                 // error ho ya na ho, connection hamesha wapas
    return n;
}

bool db_scalar(const string& sql, string& out, int* conn_id) {
    vector<vector<string>> rows;
    if (!db_select(sql, {}, rows, conn_id)) return false;
    if (rows.empty() || rows[0].empty()) return false;     // koi row / column nahi: false, crash nahi
    out = rows[0][0];
    return true;
}

// ---- request_logs: batch INSERT ----

// "0.512" jaisa text (3 decimal): latency_ms / queue_wait_ms DECIMAL(10,3) ke liye
static string ms_text(double ms) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f", ms < 0 ? 0.0 : ms);
    return buf;
}

// Kai rows ek hi multi-row INSERT me: INSERT ... VALUES (...), (...), (...). Har row ek-ek INSERT se bahut sasta
// (ek round trip, ek transaction). SQL ke tukde fixed hain, saari values ? se bound hain.
// user_id 0 = NULL (NULLIF(?, 0)). created_at asli request ka time hai (FROM_UNIXTIME), insert ka nahi.
// Return: judi hui rows, error par -1
int db_insert_request_logs(const vector<LogEntry>& rows) {
    if (rows.empty()) return 0;

    string sql = "INSERT INTO request_logs (method, path, status, latency_ms, queue_wait_ms, worker_id, user_id, created_at) VALUES ";
    vector<string> args;
    for (size_t i = 0; i < rows.size(); i++) {
        const LogEntry& e = rows[i];
        sql += (i == 0 ? "" : ", ");
        sql += "(?, ?, ?, ?, ?, ?, NULLIF(?, 0), FROM_UNIXTIME(?))";
        args.push_back(e.method);
        args.push_back(e.path);
        args.push_back(to_string(e.status));
        args.push_back(ms_text(e.latency_ms));
        args.push_back(ms_text(e.queue_wait_ms));
        args.push_back(to_string(e.worker_id));
        args.push_back(to_string(e.user_id));
        args.push_back(to_string(e.created));
    }
    return db_exec(sql, args);
}

// ---- cart ----
// Yeh functions ek diya hua connection (m) use karte hain: ek cart request ki saari queries ek hi connection par
// chalti hain (pehle get_conn, aakhir me put_conn: pages.cpp me).

// user ka ACTIVE cart dhundo (sirf SELECT, kuch banata nahi). Nahi mila ya error: 0
int find_active_cart(MYSQL* m, int user_id) {
    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT cart_id FROM cart WHERE user_id = ? AND status = 'ACTIVE'",
                      {to_string(user_id)}, rows))
        return 0;
    if (rows.empty() || rows[0].empty()) return 0;
    return atoi(rows[0][0].c_str());
}

// ACTIVE cart lo, na ho to banao. Error par 0 (crash nahi).
// Race: do request ek saath cart banane aayein to dono SELECT me "cart nahi hai" dekhte hain aur dono INSERT karte hain.
// Schema me UNIQUE(active_user_id) hai, isliye ek INSERT jeetta hai, dusre ko error 1062 (duplicate) milta hai.
// 1062 galti nahi hai: matlab cart ab hai (kisi aur ne bana diya), bas dobara SELECT karke use le lo.
int get_active_cart(MYSQL* m, int user_id) {
    int cart_id = find_active_cart(m, user_id);
    if (cart_id > 0) return cart_id;

    int err = 0;
    int n = db_exec_on(m, "INSERT INTO cart (user_id) VALUES (?)", {to_string(user_id)}, &err);
    if (n < 0 && err != 1062) return 0;         // asli error
    return find_active_cart(m, user_id);        // hamara INSERT ho gaya ya kisi aur ka, dono me ab cart hai
}

// Cart me product jodo (quantity pehle se ho to badhao), ek hi atomic query me.
//
// LOST UPDATE problem: agar hum pehle quantity padhte (SELECT -> 2), C++ me +1 karte (3), phir likhte (UPDATE ... = 3)
// to do request ek saath aane par dono 2 padhte aur dono 3 likhte: ek +1 kho jata (lost update).
// Isliye padhna aur badhana ek hi SQL statement me hai: "quantity = cart_items.quantity + ..." MySQL row par lock
// lekar CURRENT value se jodta hai, to dusri request pehli ke baad hi chalti hai aur dono jud jaate hain.
// LEAST(..., MAX) total ko MAX_CART_QTY par rok deta hai. "AS new_item" MySQL 8 ka row alias hai
// (purana VALUES(quantity) deprecated hai). MAX_CART_QTY hamara constant hai, user input nahi.
// cart_id yahan session se aaya hua hai (form se kabhi nahi). false = error
bool cart_add(MYSQL* m, int cart_id, const string& product_id, const string& qty) {
    string sql = "INSERT INTO cart_items (cart_id, product_id, quantity) VALUES (?, ?, ?) AS new_item "
                 "ON DUPLICATE KEY UPDATE quantity = LEAST(cart_items.quantity + new_item.quantity, " +
                 to_string(MAX_CART_QTY) + ")";
    return db_exec_on(m, sql, {to_string(cart_id), product_id, qty}) >= 0;
}

// quantity seedha "set" karo (qty 0 = item hata do). Yeh "= nayi value" hai, "padho-badlo-likho" nahi,
// isliye isme lost update nahi hota. Return: match hui rows (0 = item cart me tha hi nahi), error par -1
int cart_set_qty(MYSQL* m, int cart_id, const string& product_id, const string& qty) {
    if (qty == "0") return cart_remove(m, cart_id, product_id);
    return db_exec_on(m, "UPDATE cart_items SET quantity = ? WHERE cart_id = ? AND product_id = ?",
                      {qty, to_string(cart_id), product_id});
}

// item hata do. Return: hati hui rows (0 = pehle se nahi tha), error par -1
int cart_remove(MYSQL* m, int cart_id, const string& product_id) {
    return db_exec_on(m, "DELETE FROM cart_items WHERE cart_id = ? AND product_id = ?",
                      {to_string(cart_id), product_id});
}

// cart ke items: product_id, name, price, qty, stock, is_active. Price hamesha products table se (form se kabhi nahi)
bool cart_items(MYSQL* m, int cart_id, vector<vector<string>>& rows) {
    return db_select_on(m, "SELECT p.product_id, p.name, p.price, ci.quantity, p.stock, p.is_active "
                           "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id "
                           "WHERE ci.cart_id = ? ORDER BY p.name", {to_string(cart_id)}, rows);
}

// user ke ACTIVE cart me total items (quantity ka jod); cart na ho to 0, error par -1
int cart_count(MYSQL* m, int user_id) {
    vector<vector<string>> rows;
    if (!db_select_on(m, "SELECT COALESCE(SUM(ci.quantity), 0) FROM cart c "
                         "LEFT JOIN cart_items ci ON ci.cart_id = c.cart_id "
                         "WHERE c.user_id = ? AND c.status = 'ACTIVE'", {to_string(user_id)}, rows))
        return -1;
    if (rows.empty() || rows[0].empty()) return 0;
    return atoi(rows[0][0].c_str());
}

// ---- order ka raasta: PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED ----
static const char* FLOW[] = {"PLACED", "PACKED", "SHIPPED", "OUT_FOR_DELIVERY", "DELIVERED"};
const int FLOW_N = 5;
// FLOW[i] me aane se pehle ka delay (seconds): [0] khali, kyunki PLACED par order seedha banta hai
static const int FLOW_MIN[] = {0, DELAY_PACKED_MIN, DELAY_SHIPPED_MIN, DELAY_OUT_FOR_DELIVERY_MIN, DELAY_DELIVERED_MIN};
static const int FLOW_MAX[] = {0, DELAY_PACKED_MAX, DELAY_SHIPPED_MAX, DELAY_OUT_FOR_DELIVERY_MAX, DELAY_DELIVERED_MAX};

// status ka number raaste me (PLACED = 0 ... DELIVERED = 4); CANCELLED / unknown = -1
static int flow_index(const string& status) {
    for (int i = 0; i < FLOW_N; i++)
        if (status == FLOW[i]) return i;
    return -1;
}

// FLOW[i] me aane se pehle ka random delay, min..max ke beech. thread_local: har thread ka apna generator, lock nahi chahiye
static int random_delay(int i) {
    static thread_local mt19937 gen(random_device{}());
    return uniform_int_distribution<int>(FLOW_MIN[i], FLOW_MAX[i])(gen);
}

// SQL ka tukda: "agla step itni der baad" (next_step_at). Aakhri status (DELIVERED/CANCELLED) par NULL: aage kuch nahi.
// scheduled = true (delivery thread): delay PICHLE step ke tay time (purani next_step_at) se judta hai, NOW() se nahi.
// Isse server band rehne par bhi shipment ka time nahi rukta: restart par thread ek ke baad ek saare chhute steps nipta deta hai.
// scheduled = false (checkout, admin, customer): koi tay time nahi, isliye NOW() se.
// Number hamara apna int hai (user ka text nahi), isliye SQL me seedha; baaki values ? se
static string next_step_sql(const string& status, bool scheduled = false) {
    int i = flow_index(status);
    if (i < 0 || i >= FLOW_N - 1) return "NULL";
    return string(scheduled ? "DATE_ADD(next_step_at, INTERVAL " : "NOW() + INTERVAL ") + to_string(random_delay(i + 1)) + " SECOND" +
           (scheduled ? ")" : "");
}

string order_next_status(const string& status) {
    int i = flow_index(status);
    if (i < 0 || i >= FLOW_N - 1) return "";
    return FLOW[i + 1];
}

// "Arriving by" ke liye: agla step next_step_at par hoga, uske baad ke steps ka average (min+max)/2 yahan jodte hain
int order_eta_seconds(const string& status) {
    int i = flow_index(status);
    if (i < 0 || i >= FLOW_N - 1) return 0;
    int sum = 0;
    for (int k = i + 2; k < FLOW_N; k++) sum += (FLOW_MIN[k] + FLOW_MAX[k]) / 2;
    return sum;
}

// ---- checkout: ek DB transaction ----
// Transaction = kai queries ka ek "sab ya kuch nahi" group (ACID ka A, atomicity). Agar beech me kuch bhi fail ho
// to ROLLBACK se sab pehle jaisa; sab theek ho to COMMIT se sab ek saath pakka. Checkout me yeh zaroori hai:
// order ban jaye par stock kam na ho, ya stock kam ho jaye par payment na bane, aisa kabhi nahi hona chahiye.

const int CO_OK = 0;        // sab ho gaya (abhi COMMIT baaki)
const int CO_PROBLEM = 1;   // user wali baat (cart khali, stock kam): ROLLBACK, message dikhao
const int CO_RETRY = 2;     // deadlock / lock wait timeout: ROLLBACK aur dobara try
const int CO_FAIL = 3;      // koi aur error: ROLLBACK, error page

// transaction shuru: autocommit band (warna har query khud COMMIT ho jaati) aur START TRANSACTION
static bool db_begin(MYSQL* m) {
    if (mysql_autocommit(m, false) || mysql_query(m, "START TRANSACTION") != 0) {
        log_tag("CHECKOUT", "cannot start transaction: " + string(mysql_error(m)));
        return false;
    }
    return true;
}

// autocommit wapas ON. Pool ki connection kisi aur request ko milegi, usse autocommit band nahi milna chahiye.
// NOTE: autocommit ON karna chalti transaction ko COMMIT kar deta hai, isliye isse pehle ROLLBACK ya COMMIT ho chuka hona chahiye
static void db_end(MYSQL* m) {
    if (mysql_autocommit(m, true) && mysql_query(m, "SET autocommit = 1") != 0)
        log_tag("CHECKOUT", "BUG: could not turn autocommit back on: " + string(mysql_error(m)));
}

// 1213 = deadlock (InnoDB ne hamari transaction hata di), 1205 = lock wait timeout: dobara try karne layak
static int fail_code(int err, CheckoutResult& r, const string& why) {
    r.message = why;
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

// ek query chalao jo ek row badle (n == 1 chahiye). Warna error
static bool exec_one(MYSQL* m, const string& sql, const vector<string>& args, int& err) {
    return db_exec_on(m, sql, args, &err) == 1;
}

// Transaction ke andar ka kaam (db_begin ho chuka hai). COMMIT/ROLLBACK yeh nahi karta, caller karta hai.
static int checkout_steps(MYSQL* m, int user_id, int address_id, const PaymentInfo& pay, CheckoutResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;

    // STEP 1: user ka ACTIVE cart FOR UPDATE se lock. Ab isi user ki dusri checkout request yahin ruk jayegi
    // (double click / do tab se do order nahi ban sakte). Cart user ke session se aata hai, form se nahi.
    if (!db_select_on(m, "SELECT cart_id FROM cart WHERE user_id = ? AND status = 'ACTIVE' FOR UPDATE",
                      {to_string(user_id)}, rows, &err))
        return fail_code(err, r, "Database error");
    if (rows.empty() || rows[0].empty()) { r.message = "Your cart is empty."; return CO_PROBLEM; }
    string cart_id = rows[0][0];

    // STEP 1b: shipping address. Sirf isi user ka aur active (soft-delete nahi hua). FOR SHARE: check ke baad jab tak hum
    // is transaction me hain, koi is row ko badal / delete nahi kar sakta. Nahi mila = 404 (doosre ka address bhi "mila hi nahi")
    if (!db_select_on(m, "SELECT address_id FROM addresses WHERE address_id = ? AND user_id = ? AND is_active = TRUE FOR SHARE",
                      {to_string(address_id), to_string(user_id)}, rows, &err))
        return fail_code(err, r, "Database error");
    if (rows.empty()) { r.status = 404; r.message = "That shipping address was not found. Please choose another."; return CO_PROBLEM; }

    // STEP 2: cart ke items + products ko FOR UPDATE se lock. ORDER BY product_id: sab log hamesha chhote
    // product_id se bada ki taraf lock lete hain. Agar ek 1->2 aur dusra 2->1 lock leta to dono ek dusre ka
    // intezaar karte (DEADLOCK); same order se aisa nahi hota. Lock ke baad stock koi aur badal nahi sakta.
    vector<vector<string>> items;       // product_id, name, price, quantity, stock, is_active (product_id ke order me)
    if (!db_select_on(m, "SELECT p.product_id, p.name, p.price, ci.quantity, p.stock, p.is_active "
                         "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id "
                         "WHERE ci.cart_id = ? ORDER BY p.product_id FOR UPDATE",
                      {cart_id}, items, &err))
        return fail_code(err, r, "Database error");
    if (items.empty()) { r.message = "Your cart is empty."; return CO_PROBLEM; }
    for (auto& row : items)
        if (row.size() < 6) return fail_code(0, r, "Database error");

    // (sirf testing) lock pakad kar ruko, taaki do checkout ek saath chalane par race saaf dikhe
    if (CHECKOUT_DELAY_MS > 0) this_thread::sleep_for(chrono::milliseconds(CHECKOUT_DELAY_MS));

    // STEP 3: ab stock sach me check karo. Rows locked hain, isliye jo stock dikh raha hai wahi rahega.
    for (auto& row : items) {
        int qty = atoi(row[3].c_str());
        int stock = atoi(row[4].c_str());
        // admin ne product hide kar diya (rows locked hain: ab badlega nahi): order nahi banega, sab ROLLBACK
        if (row[5] == "0") { r.message = row[1] + " is no longer available."; return CO_PROBLEM; }
        if (stock == 0) { r.message = row[1] + " is out of stock."; return CO_PROBLEM; }
        if (qty > stock) {
            r.message = "Only " + to_string(stock) + " of " + row[1] + " available, but you have " +
                        to_string(qty) + " in your cart.";
            return CO_PROBLEM;
        }
    }

    // STEP 4: order banao. Total DB ke price se hi (price*quantity ka SUM), form/cart se kabhi nahi.
    // next_step_at = abhi + random delay: delivery thread isi time par order ko PACKED karega
    if (!exec_one(m, "INSERT INTO orders (user_id, cart_id, total, status, next_step_at) "
                     "SELECT ?, ?, SUM(p.price * ci.quantity), 'PLACED', " + next_step_sql("PLACED") + " "
                     "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id WHERE ci.cart_id = ?",
                  {to_string(user_id), cart_id, cart_id}, err))
        return fail_code(err, r, "Database error");
    // is connection par abhi bani order ki id (har connection ki apni LAST_INSERT_ID hoti hai)
    if (!db_select_on(m, "SELECT LAST_INSERT_ID()", {}, rows, &err) || rows.empty() || rows[0][0] == "0")
        return fail_code(err, r, "Database error");
    string order_id = rows[0][0];
    r.order_id = atoi(order_id.c_str());

    // status history ki pehli row (PLACED), usi transaction me: order ban gaya to history bhi, rollback to dono gayab
    if (!exec_one(m, "INSERT INTO order_status_history (order_id, status, changed_by) VALUES (?, 'PLACED', 'CUSTOMER')",
                  {order_id}, err))
        return fail_code(err, r, "Database error");

    // order ka address: SNAPSHOT copy (state ka NAAM text me). Baad me user address badle / delete kare to bhi
    // is order ka address wahi rehta hai, bilkul order_items ki price ki tarah
    if (!exec_one(m, "INSERT INTO order_addresses (order_id, first_name, last_name, phone, line1, line2, landmark, city, "
                     "state_name, pincode, address_type) "
                     "SELECT ?, a.first_name, a.last_name, a.phone, a.line1, a.line2, a.landmark, a.city, s.state_name, "
                     "a.pincode, a.address_type FROM addresses a JOIN states s ON s.state_id = a.state_id "
                     "WHERE a.address_id = ? AND a.user_id = ?",
                  {order_id, to_string(address_id), to_string(user_id)}, err))
        return fail_code(err, r, "Database error");

    // (sirf testing) yahin error: order INSERT ho chuka hai, ROLLBACK usse wapas hata dena chahiye
    if (TEST_FAIL_CHECKOUT) return fail_code(0, r, "Test failure after INSERT orders");

    // STEP 5: order_items: har item ki price ABHI ki copy (price at purchase time), taaki baad me products ki
    // price badle to purani order ka hisaab na badle. Kitni rows judi = items ki ginti honi chahiye.
    int n = db_exec_on(m, "INSERT INTO order_items (order_id, product_id, quantity, price) "
                          "SELECT ?, p.product_id, ci.quantity, p.price "
                          "FROM cart_items ci JOIN products p ON p.product_id = ci.product_id WHERE ci.cart_id = ?",
                       {order_id, cart_id}, &err);
    if (n < 0) return fail_code(err, r, "Database error");
    if (n != (int)items.size()) return fail_code(0, r, "Database error");

    // STEP 6: stock kam karo, product_id ke order me. "AND stock >= ?" aakhri suraksha hai: stock kabhi minus nahi
    // jaayega (table me CHECK(stock >= 0) bhi hai). Ek row badalni chahiye, warna error.
    for (auto& it : items) {
        if (!exec_one(m, "UPDATE products SET stock = stock - ? WHERE product_id = ? AND stock >= ?",
                      {it[3], it[0], it[3]}, err))
            return fail_code(err, r, err == 0 ? "Stock changed, please try again." : "Database error");
    }

    // STEP 7: payment. Gateway ka "charge" order aur stock lock hone ke BAAD, COMMIT se pehle: bank mana kare to CO_PROBLEM ->
    // ROLLBACK, yaani order, order_items, stock sab wapas, jaise kuch hua hi nahi (paise bhi nahi kate)
    if (pay.declined) {
        r.message = "Payment declined by the bank. No money was taken and the order was not placed.";
        return CO_PROBLEM;
    }
    // COD: PENDING, paid_at NULL (paise delivery par). Online: abhi PAID + paid_at + txn_ref, isi transaction me
    bool online = pay.method != "COD";
    if (!exec_one(m, "INSERT INTO payments (order_id, amount, method, pay_detail, txn_ref, status, paid_at) "
                     "SELECT order_id, total, ?, NULLIF(?, ''), NULLIF(?, ''), ?, " + string(online ? "NOW()" : "NULL") +
                     " FROM orders WHERE order_id = ?",
                  {pay.method, pay.detail, pay.txn_ref, online ? "PAID" : "PENDING", order_id}, err))
        return fail_code(err, r, "Database error");

    // STEP 8: cart ab CHECKED_OUT (purana cart band; agli baar naya ACTIVE cart banega)
    if (!exec_one(m, "UPDATE cart SET status = 'CHECKED_OUT' WHERE cart_id = ?", {cart_id}, err))
        return fail_code(err, r, "Database error");

    if (!db_select_on(m, "SELECT total FROM orders WHERE order_id = ?", {order_id}, rows, &err) ||
        rows.empty() || rows[0].empty())
        return fail_code(err, r, "Database error");
    r.total = rows[0][0];
    return CO_OK;
}

// Ek poori koshish: BEGIN -> steps -> COMMIT (ya ROLLBACK). Return: CO_* code, r me status/message
static int checkout_attempt(MYSQL* m, int user_id, int address_id, const PaymentInfo& pay, CheckoutResult& r) {
    string who = "user " + to_string(user_id);
    if (!db_begin(m)) {
        r.status = 500;
        r.message = "Database error";
        return CO_FAIL;
    }

    int err = 0;
    int code = checkout_steps(m, user_id, address_id, pay, r, err);

    // STEP 9: sab theek raha to COMMIT: ab saare changes ek saath pakke aur dusron ko dikhte hain
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = fail_code(err, r, "Could not save the order");
    }

    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        metric_checkout(true);                  // /metrics ke liye COMMIT ginti
        log_tag("CHECKOUT", who + " order " + to_string(r.order_id) + " total " + r.total + " COMMIT");
        return code;
    }

    // Kisi bhi problem par ROLLBACK: is transaction ne jo bhi badla (order, stock, payment...) sab wapas.
    // (deadlock me InnoDB khud rollback kar chuka hota hai; dobara rollback karna nuksan nahi)
    mysql_rollback(m);
    metric_checkout(false);                     // /metrics ke liye ROLLBACK ginti
    r.status = (code == CO_PROBLEM) ? (r.status == 404 ? 404 : 409) : 500;   // 404 = address nahi mila (steps ne set kiya)
    string order = r.order_id > 0 ? " order " + to_string(r.order_id) : "";
    log_tag("CHECKOUT", who + order + " ROLLBACK reason: " + r.message +
                        (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    r.order_id = 0;                     // rollback ho gaya: woh order exist nahi karta
    return code;
}

CheckoutResult do_checkout(int user_id, int address_id, const PaymentInfo& pay) {
    CheckoutResult r;
    int id = 0;
    MYSQL* m = get_conn(id);            // poora checkout isi ek connection par (transaction ek connection ki hoti hai)
    if (m == nullptr) return r;

    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = CheckoutResult();
        int code = checkout_attempt(m, user_id, address_id, pay, r);
        if (code != CO_RETRY) break;

        // deadlock / timeout: poori transaction shuru se dobara (ROLLBACK ho chuka hai, kuch adhura nahi bacha)
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
            log_tag("CHECKOUT", "user " + to_string(user_id) + " gave up after " + to_string(attempt) + " attempts");
        } else {
            log_tag("CHECKOUT", "user " + to_string(user_id) + " retry " + to_string(attempt + 1) + "/" +
                                to_string(CHECKOUT_RETRIES));
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));   // thoda ruko, dusri transaction nipat jaye
        }
    }

    db_end(m);                          // autocommit wapas ON: hamesha, error ho ya na ho
    put_conn(id);
    return r;
}

// ---- shipping addresses ----

// text ke characters ki ginti (bytes nahi): MySQL VARCHAR(n) bhi characters ginta hai. UTF-8 ka continuation byte (10xxxxxx) nahi ginte
static size_t addr_chars(const string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

// sirf digits? (aur kitne)
static bool all_digits(const string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// koi control character (newline, tab, 0x00...) to nahi? Address ek line ka text hai
static bool has_control(const string& s) {
    for (unsigned char c : s)
        if (c < 32 || c == 127) return true;
    return false;
}

// ek text field ki jaanch: required, max length, no control chars. Galti par message, theek hone par ""
static string addr_text_check(const string& label, const string& v, size_t max_chars, bool required) {
    if (v.empty()) return required ? label + " is required." : "";
    if (addr_chars(v) > max_chars) return label + " can have at most " + to_string(max_chars) + " characters.";
    if (has_control(v)) return label + " has characters that are not allowed.";
    return "";
}

// pura form: pehle trim, phir ek-ek field. Pehli galti ka message (taaki user ek-ek karke theek kare). "" = sab theek.
// Yahi rules DB ke CHECK me bhi hain (phone 10 digits, pincode 6 digits 0 se shuru nahi, type teen me se ek): yeh aakhri suraksha hai
string address_check(Address& a) {
    a.first_name = trim(a.first_name);
    a.last_name = trim(a.last_name);
    a.phone = trim(a.phone);
    a.line1 = trim(a.line1);
    a.line2 = trim(a.line2);
    a.landmark = trim(a.landmark);
    a.city = trim(a.city);
    a.state_id = trim(a.state_id);
    a.pincode = trim(a.pincode);
    a.type = trim(a.type);

    string e;
    if (!(e = addr_text_check("First name", a.first_name, 50, true)).empty()) return e;
    if (!(e = addr_text_check("Last name", a.last_name, 50, true)).empty()) return e;
    if (!all_digits(a.phone, 10)) return "Phone number must be exactly 10 digits (numbers only).";
    if (!(e = addr_text_check("Address line 1", a.line1, 150, true)).empty()) return e;
    if (!(e = addr_text_check("Address line 2", a.line2, 150, false)).empty()) return e;
    if (!(e = addr_text_check("Landmark", a.landmark, 100, false)).empty()) return e;
    if (!(e = addr_text_check("City", a.city, 80, true)).empty()) return e;
    if (a.state_id.empty() || a.state_id.size() > 9 || !all_digits(a.state_id, a.state_id.size())) return "Please choose a state.";
    if (!all_digits(a.pincode, 6) || a.pincode[0] == '0') return "Pincode must be 6 digits and cannot start with 0.";
    if (a.type != "HOME" && a.type != "WORK" && a.type != "OTHER") return "Address type must be Home, Work or Other.";
    return "";
}

// address ke kaam: checkout jaisa hi ek connection, ek transaction. Code CO_* db.cpp ke upar wale hain
static int addr_fail(int err, AddressResult& r) {
    r.status = (err == 1213 || err == 1205) ? 503 : 500;
    r.message = r.status == 503 ? "The shop is busy right now. Please try again in a moment." : "Database error";
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

static int addr_problem(AddressResult& r, int status, const string& message) {
    r.status = status;
    r.message = message;
    return CO_PROBLEM;
}

// naya address / edit. Transaction me:
//  1) users ki apni row FOR UPDATE: is user ke address badalne wale sab ek line me (do tab se ek saath "Add" dabane par
//     dono limit dekh kar 11 address na bana de). Is user ki row ko aur koi lock nahi karta, isliye deadlock ka darr nahi.
//  2) limit check, 3) INSERT / UPDATE. Pehla address apne aap default ban jata hai.
static int addr_save_steps(MYSQL* m, int user_id, const Address& a, bool is_new, AddressResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;
    const string uid = to_string(user_id);

    if (!db_select_on(m, "SELECT user_id FROM users WHERE user_id = ? FOR UPDATE", {uid}, rows, &err)) return addr_fail(err, r);
    if (rows.empty()) return addr_problem(r, 404, "User not found");

    if (is_new) {
        if (!db_select_on(m, "SELECT COUNT(*), COALESCE(SUM(is_default), 0) FROM addresses WHERE user_id = ? AND is_active = TRUE",
                          {uid}, rows, &err) || rows.empty() || rows[0].size() < 2)
            return addr_fail(err, r);
        if (atoi(rows[0][0].c_str()) >= MAX_ADDRESSES)
            return addr_problem(r, 400, "You can save at most " + to_string(MAX_ADDRESSES) + " addresses. Delete one first.");
        string first = (rows[0][1] == "0") ? "1" : "0";         // abhi koi default nahi to yeh default

        int n = db_exec_on(m, "INSERT INTO addresses (user_id, first_name, last_name, phone, line1, line2, landmark, city, "
                              "state_id, pincode, address_type, is_default) VALUES (?, ?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), ?, ?, ?, ?, ?)",
                           {uid, a.first_name, a.last_name, a.phone, a.line1, a.line2, a.landmark, a.city, a.state_id, a.pincode,
                            a.type, first}, &err);
        if (err == 1452) return addr_problem(r, 400, "Please choose a valid state.");   // foreign key: aisa state nahi
        if (n != 1) return addr_fail(err, r);
        if (!db_select_on(m, "SELECT LAST_INSERT_ID()", {}, rows, &err) || rows.empty() || rows[0].empty()) return addr_fail(err, r);
        r.address_id = atoi(rows[0][0].c_str());
        return CO_OK;
    }

    // edit: "AND user_id = ? AND is_active": kisi aur ka ya delete ho chuka address 0 rows = 404
    int n = db_exec_on(m, "UPDATE addresses SET first_name = ?, last_name = ?, phone = ?, line1 = ?, line2 = NULLIF(?, ''), "
                          "landmark = NULLIF(?, ''), city = ?, state_id = ?, pincode = ?, address_type = ? "
                          "WHERE address_id = ? AND user_id = ? AND is_active = TRUE",
                       {a.first_name, a.last_name, a.phone, a.line1, a.line2, a.landmark, a.city, a.state_id, a.pincode, a.type,
                        a.id, uid}, &err);
    if (err == 1452) return addr_problem(r, 400, "Please choose a valid state.");
    if (n < 0) return addr_fail(err, r);
    if (n == 0) return addr_problem(r, 404, "Address not found");
    r.address_id = atoi(a.id.c_str());
    return CO_OK;
}

// set default: is user ke saare active addresses FOR UPDATE (address_id ke order me), phir purana default hatao,
// naya lagao. Dono UPDATE ek transaction me: beech me kabhi "do default" ya (crash par) "ek bhi nahi" nahi dikhta.
// (UNIQUE(default_user) bhi do default ko rokta hai; isliye pehle hatana zaroori hai, phir lagana)
static int addr_default_steps(MYSQL* m, int user_id, int address_id, AddressResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;
    const string uid = to_string(user_id);

    if (!db_select_on(m, "SELECT address_id FROM addresses WHERE user_id = ? AND is_active = TRUE ORDER BY address_id FOR UPDATE",
                      {uid}, rows, &err))
        return addr_fail(err, r);
    bool found = false;
    for (auto& row : rows)
        if (!row.empty() && atoi(row[0].c_str()) == address_id) found = true;
    if (!found) return addr_problem(r, 404, "Address not found");

    if (db_exec_on(m, "UPDATE addresses SET is_default = FALSE WHERE user_id = ? AND is_default = TRUE", {uid}, &err) < 0)
        return addr_fail(err, r);
    if (db_exec_on(m, "UPDATE addresses SET is_default = TRUE WHERE address_id = ? AND user_id = ?",
                   {to_string(address_id), uid}, &err) != 1)
        return addr_fail(err, r);
    r.address_id = address_id;
    return CO_OK;
}

// ek transaction chalao: BEGIN -> steps -> COMMIT (ya ROLLBACK). which: 0 = save, 1 = set default
static int addr_attempt(MYSQL* m, int which, int user_id, const Address& a, bool is_new, int address_id, AddressResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        return CO_FAIL;
    }
    int err = 0;
    int code = (which == 0) ? addr_save_steps(m, user_id, a, is_new, r, err) : addr_default_steps(m, user_id, address_id, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = addr_fail(err, r);
    }
    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        return code;
    }
    mysql_rollback(m);
    log_tag("DB", "address change of user " + to_string(user_id) + " ROLLBACK reason: " + r.message +
                  (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

static AddressResult addr_run(int which, int user_id, const Address& a, bool is_new, int address_id) {
    AddressResult r;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return r;
    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = AddressResult();
        int code = addr_attempt(m, which, user_id, a, is_new, address_id, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
        } else {
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }
    db_end(m);                          // autocommit wapas ON: hamesha
    put_conn(id);
    return r;
}

AddressResult address_save(int user_id, const Address& a, bool is_new) {
    return addr_run(0, user_id, a, is_new, 0);
}

AddressResult address_set_default(int user_id, int address_id) {
    return addr_run(1, user_id, Address(), false, address_id);
}

// delete = is_active = 0 (soft delete): row rehti hai, purane orders ka address (order_addresses) to alag copy hai hi.
// Default tha to is_default bhi 0: ab user ka koi default nahi (checkout par pehla address pehle se chuna dikhta hai).
// Ek hi UPDATE hai, isliye transaction ki zaroorat nahi (ek statement khud atomic hai)
AddressResult address_delete(int user_id, int address_id) {
    AddressResult r;
    int err = 0;
    int n = db_exec("UPDATE addresses SET is_active = FALSE, is_default = FALSE WHERE address_id = ? AND user_id = ? AND is_active = TRUE",
                    {to_string(address_id), to_string(user_id)}, &err);
    if (n < 0) return r;
    if (n == 0) { r.status = 404; r.message = "Address not found"; return r; }
    r.status = 200;
    r.message = "";
    r.address_id = address_id;
    return r;
}

// ---- password badalna: ek DB transaction ----
// Password abhi plain text me hi store hota hai (hash baad me). Niyam (8-64 characters, upper/lower/digit/special ...) pages.cpp ka
// check_password dekhta hai, DB ke CHECK me nahi: wo sirf typed password par lagte hain, hash hone par DB me wo shakal rahegi hi nahi.
static int pw_fail(int err, PasswordResult& r) {
    r.status = (err == 1213 || err == 1205) ? 503 : 500;
    r.message = r.status == 503 ? "The shop is busy right now. Please try again in a moment." : "Database error";
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

// Transaction ke andar:
//  1) UPDATE ... WHERE user_id = ? AND BINARY password = <purana>: "purana sahi hai" ki jaanch aur badlav ek hi statement me, to beech me
//     kisi aur ke badalne ka gap nahi. 0 rows = purana password galat. (BINARY: capital/small ka farq, login jaisa)
//  2) is user ke baaki sab active sessions band (end_reason 'LOGOUT'), sirf abhi wala session (keep_session) chalta rehta hai.
//  Dono saath hi pakke hote hain ya saath hi nahi: password badle aur purane session zinda rahein, aisa kabhi nahi
static int pw_steps(MYSQL* m, int user_id, const string& current, const string& new_pw, const string& keep_session, PasswordResult& r, int& err) {
    err = 0;
    int n = db_exec_on(m, "UPDATE users SET password = ? WHERE user_id = ? AND BINARY password = ?",
                       {new_pw, to_string(user_id), current}, &err);
    if (n < 0) return pw_fail(err, r);
    if (n == 0) {
        r.status = 403;
        r.message = "Current password is incorrect.";
        return CO_PROBLEM;
    }
    int ended = db_exec_on(m, "UPDATE user_sessions SET is_active = 0, logout_time = NOW(), end_reason = 'LOGOUT' "
                              "WHERE user_id = ? AND is_active = 1 AND session_id <> ?",
                           {to_string(user_id), keep_session}, &err);
    if (ended < 0) return pw_fail(err, r);
    r.ended = ended;
    return CO_OK;
}

static int pw_attempt(MYSQL* m, int user_id, const string& current, const string& new_pw, const string& keep_session, PasswordResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        return CO_FAIL;
    }
    int err = 0;
    int code = pw_steps(m, user_id, current, new_pw, keep_session, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = pw_fail(err, r);
    }
    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        return code;
    }
    mysql_rollback(m);
    // password kabhi log me nahi: sirf kis user ka kaam aur kyun nahi hua
    log_tag("DB", "password change of user " + to_string(user_id) + " ROLLBACK reason: " + r.message +
                  (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

PasswordResult password_change(int user_id, const string& current, const string& new_pw, const string& keep_session) {
    PasswordResult r;
    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return r;
    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = PasswordResult();
        int code = pw_attempt(m, user_id, current, new_pw, keep_session, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
        } else {
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }
    db_end(m);                          // autocommit wapas ON: hamesha
    put_conn(id);
    return r;
}

// ---- order status badalna (admin: ship / deliver / cancel, customer: cancel): ek DB transaction ----
// Allowed raaste (aur kuch nahi): ek step aage (PLACED->PACKED->SHIPPED->OUT_FOR_DELIVERY->DELIVERED), ya DELIVERED se
// pehle kabhi bhi CANCELLED. Customer sirf PLACED / PACKED se cancel kar sakta hai (order_steps me).
// DELIVERED aur CANCELLED ke baad order kabhi nahi badalta.
bool order_transition_ok(const string& from, const string& to) {
    int f = flow_index(from);
    if (f < 0) return false;                    // CANCELLED / unknown se kahin nahi
    if (to == "CANCELLED") return f < FLOW_N - 1;
    return flow_index(to) == f + 1;
}

// checkout jaisa hi: deadlock / lock timeout par dobara try, baaki error par error page
static int order_fail(int err, OrderResult& r, const string& why) {
    r.status = 500;
    r.message = why;
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

// user wali baat (order nahi mila, raasta allowed nahi): ROLLBACK aur status + message dikhao
static int order_problem(OrderResult& r, int status, const string& message) {
    r.status = status;
    r.message = message;
    return CO_PROBLEM;
}

// Transaction ke andar ka kaam (db_begin ho chuka hai). COMMIT/ROLLBACK caller karta hai.
static int order_steps(MYSQL* m, int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                       const string& by, OrderResult& r, int& err) {
    vector<vector<string>> rows;
    err = 0;
    const string oid = to_string(order_id);

    // STEP 1: order ko FOR UPDATE se lock. Ab isi order ko badalne wali dusri request (dusra admin, ya customer ka cancel)
    // yahin ruk jayegi, aur lock milne ke baad jo status dikhega wahi sach hoga
    if (!db_select_on(m, "SELECT status, user_id, IFNULL(DATE_FORMAT(next_step_at, '%Y-%m-%d %H:%i:%s'), '') FROM orders WHERE order_id = ? FOR UPDATE", {oid}, rows, &err))
        return order_fail(err, r, "Database error");
    if (rows.empty() || rows[0].size() < 3) return order_problem(r, 404, "Order not found");
    const string from = rows[0][0];
    r.from = from;
    // delivery thread ka step: yeh step apne TAY time par hua mana jayega (changed_at, paid_at, agla next_step_at isi se),
    // chahe thread late chali ho. Baaki (admin/customer) ke liye tay time nahi, "abhi" hi sach hai
    const string sched = (by == "SYSTEM") ? rows[0][2] : "";

    // STEP 2: checks. Customer sirf apna order dekh/cancel kar sakta hai (doosre ka order "mila hi nahi" jaisa), aur sirf
    // PLACED ya PACKED wala (uske baad saaman nikal chuka). Server hi decide karta hai: page ke button sirf suvidha hain
    if (!as_admin && atoi(rows[0][1].c_str()) != user_id) return order_problem(r, 404, "Order not found");
    if (!expected_from.empty() && expected_from != from)
        return order_problem(r, 409, "This order was changed by someone else (now " + from + "). Reload the page and check it.");
    if (!order_transition_ok(from, to) || (!as_admin && !((from == "PLACED" || from == "PACKED") && to == "CANCELLED")))
        return order_problem(r, 400, "Not allowed: an order cannot go from " + from + " to " + to + ".");

    if (to == "CANCELLED") {
        // STEP 3: har item ki quantity stock me wapas, product_id ke order me (checkout bhi isi order me lock leta hai:
        // sab hamesha chhote id se bade ki taraf, to deadlock nahi banta)
        vector<vector<string>> items;
        if (!db_select_on(m, "SELECT product_id, quantity FROM order_items WHERE order_id = ? ORDER BY product_id",
                          {oid}, items, &err))
            return order_fail(err, r, "Database error");
        for (auto& it : items) {
            if (it.size() < 2) return order_fail(0, r, "Database error");
            if (!exec_one(m, "UPDATE products SET stock = stock + ? WHERE product_id = ?", {it[1], it[0]}, err))
                return order_fail(err, r, "Database error");
        }
    }

    // STEP 4: status badlo aur agle step ka time (next_step_at) bhi. "AND status = <purana>": agar kisi tarah status badal
    // chuka ho to 0 rows, aur hum ROLLBACK karte hain
    if (!exec_one(m, "UPDATE orders SET status = ?, next_step_at = " + next_step_sql(to, !sched.empty()) + " WHERE order_id = ? AND status = ?",
                  {to, oid, from}, err))
        return order_fail(err, r, err == 0 ? "The order changed, please try again." : "Database error");

    // STEP 4b: history ki row usi transaction me: status badla to history bhi, nahi to dono nahi (changed_by = kisne badla)
    // (changed_at: tay time ho to wahi, warna DEFAULT = abhi)
    bool hist_ok = sched.empty()
        ? exec_one(m, "INSERT INTO order_status_history (order_id, status, changed_by) VALUES (?, ?, ?)", {oid, to, by}, err)
        : exec_one(m, "INSERT INTO order_status_history (order_id, status, changed_at, changed_by) VALUES (?, ?, ?, ?)",
                   {oid, to, sched, by}, err);
    if (!hist_ok) return order_fail(err, r, "Database error");

    // STEP 5: payment usi transaction me. Beech ke steps me payment nahi badalta.
    // DELIVERED: COD ke paise ab mile (PENDING -> PAID + paid_at). Online pehle se PAID hai: "AND status = 'PENDING'" ki
    // wajah se uska asli paid_at nahi badalta (0 rows bhi theek, isliye exec_one nahi).
    // CANCELLED: online ke paise kat chuke the -> REFUNDED (wapas). COD ke paise mile hi nahi the -> CANCELLED.
    if (to == "DELIVERED") {
        int n = sched.empty()
            ? db_exec_on(m, "UPDATE payments SET status = 'PAID', paid_at = NOW() WHERE order_id = ? AND status = 'PENDING'", {oid}, &err)
            : db_exec_on(m, "UPDATE payments SET status = 'PAID', paid_at = ? WHERE order_id = ? AND status = 'PENDING'", {sched, oid}, &err);
        if (n < 0) return order_fail(err, r, "Database error");
    } else if (to == "CANCELLED") {
        if (db_exec_on(m, "UPDATE payments SET status = IF(status = 'PAID', 'REFUNDED', 'CANCELLED') WHERE order_id = ?",
                       {oid}, &err) < 0)
            return order_fail(err, r, "Database error");
    }

    // STEP 6: admin ka badlav ho to audit row bhi isi transaction me (badlav aur uska record saath hi pakke hote hain ya saath hi nahi)
    if (by == "ADMIN" && !exec_one(m, "INSERT INTO admin_actions (admin_id, action, details) VALUES (?, 'ORDER_STATUS', ?)",
                              {to_string(user_id), "order " + oid + " " + from + "->" + to}, err))
        return order_fail(err, r, "Database error");
    return CO_OK;
}

static int order_attempt(MYSQL* m, int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                         const string& by, OrderResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        r.message = "Database error";
        return CO_FAIL;
    }
    int err = 0;
    int code = order_steps(m, order_id, to, user_id, as_admin, expected_from, by, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = order_fail(err, r, "Could not save the change");
    }
    if (code == CO_OK) {
        r.status = 200;
        r.message = "";
        if (to == "CANCELLED")
            log_tag("ORDER", "order " + to_string(order_id) + " CANCELLED by " + by + " " + to_string(user_id) + ", stock restored");
        return code;
    }
    mysql_rollback(m);                  // kuch bhi adhura nahi: stock, status, payment sab pehle jaisa
    log_tag("ORDER", "order " + to_string(order_id) + " -> " + to + " ROLLBACK reason: " + r.message +
                     (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

OrderResult order_change_by(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                            const string& changed_by) {
    OrderResult r;
    int id = 0;
    MYSQL* m = get_conn(id);            // poori transaction isi ek connection par
    if (m == nullptr) return r;

    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = OrderResult();
        int code = order_attempt(m, order_id, to, user_id, as_admin, expected_from, changed_by, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.message = "The shop is busy right now. Please try again in a moment.";
            log_tag("ORDER", "order " + to_string(order_id) + " gave up after " + to_string(attempt) + " attempts");
        } else {
            log_tag("ORDER", "order " + to_string(order_id) + " retry " + to_string(attempt + 1) + "/" + to_string(CHECKOUT_RETRIES));
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }

    db_end(m);                          // autocommit wapas ON: hamesha
    put_conn(id);
    return r;
}

// pages.cpp ka rasta: kaun badal raha hai wo as_admin se pata chalta hai
OrderResult order_change(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from) {
    return order_change_by(order_id, to, user_id, as_admin, expected_from, as_admin ? "ADMIN" : "CUSTOMER");
}

// delivery thread ka kaam: jin orders ka next_step_at aa gaya (ya nikal gaya, jaise server band tha) unhe ek step aage.
// Ek order = ek chhoti transaction (order_change_by ke andar), isliye ek order fail ho to baaki nahi rukte.
// UPDATE ... WHERE status = <purana> (expected_from): beech me customer ne cancel kar diya ho to 409 aata hai aur hum chhod dete hain
int deliver_due_orders() {
    vector<vector<string>> rows;
    if (!db_select("SELECT order_id, status FROM orders WHERE status NOT IN ('DELIVERED', 'CANCELLED') "
                   "AND next_step_at <= NOW() ORDER BY next_step_at LIMIT 50", {}, rows))
        return -1;

    int moved = 0;
    for (auto& row : rows) {
        if (row.size() < 2) continue;
        // jab tak is order ka agla step due hai (next_step_at <= NOW()) aage badhao: server band tha to kai step ek tick me.
        // Har step apni alag chhoti transaction. FLOW_N se zyada nahi (order ka raasta itna hi lamba hai)
        string from = row[1], now_at = row[1];
        int steps = 0;
        for (int k = 0; k < FLOW_N; k++) {
            string to = order_next_status(now_at);
            if (to.empty()) break;
            OrderResult r = order_change_by(atoi(row[0].c_str()), to, 0, true, now_at, "SYSTEM");
            if (r.status != 200) {
                if (r.status != 409)        // 409 = beech me kisi ne badal diya: normal, agle tick par dekhenge
                    log_tag("DELIVERY", "order " + row[0] + " " + now_at + "->" + to + " failed: " + r.message);
                break;
            }
            steps++;
            moved++;
            if (steps == 1) log_tag("DELIVERY", "order " + row[0] + " " + now_at + "->" + to);
            now_at = to;

            vector<vector<string>> again;   // agla step bhi due hai?
            if (!db_select("SELECT status FROM orders WHERE order_id = ? AND status NOT IN ('DELIVERED', 'CANCELLED') "
                           "AND next_step_at <= NOW()", {row[0]}, again) || again.empty() || again[0].empty())
                break;
        }
        if (steps > 1)
            log_tag("DELIVERY", "order " + row[0] + " caught up " + to_string(steps) + " steps after restart (" + from + "->" + now_at + ")");
    }
    return moved;
}

// ---- admin audit: har admin badlav ki ek row (admin_actions table) ----
// Kaat ke character ginti se (bytes se nahi): beech me UTF-8 character na tute, warna MySQL error deta hai
static string cut_chars(const string& s, size_t max_chars) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (chars == max_chars) break;
        chars++;
        i += len;
    }
    return s.substr(0, i);
}

// Jin badlavon me transaction nahi hai (product, category, session end), unke baad yeh. Row na likh paye to sirf log
// (badlav ho chuka hai); transaction wale badlav (order status, role) row usi transaction me likhte hain
void admin_audit(int admin_id, const string& action, const string& details) {
    int err = 0;
    int n = db_exec("INSERT INTO admin_actions (admin_id, action, details) VALUES (?, ?, ?)",
                    {to_string(admin_id), cut_chars(action, 50), cut_chars(details, 255)}, &err);
    if (n != 1) log_tag("ADMIN", "could not write the audit row (MySQL error " + to_string(err) + "): " + action + " " + details);
}

// ---- user ka role badalna: ek DB transaction ----
// Niyam: admin apna role nahi badal sakta; aakhri ADMIN kabhi demote nahi hota. Sab ADMIN rows FOR UPDATE se lock hoti hain
// aur ginti lock ke baad hoti hai: do admin ek dusre ko ek saath demote karein to ek ko lock par ruk kar naya sach dikhta hai,
// isliye "zero admin" kabhi nahi bachta.
static int role_fail(int err, RoleResult& r) {
    r.status = 500;
    r.code = "db_error";
    return (err == 1213 || err == 1205) ? CO_RETRY : CO_FAIL;
}

static int role_problem(RoleResult& r, int status, const string& code) {
    r.status = status;
    r.code = code;
    return CO_PROBLEM;
}

static int role_steps(MYSQL* m, int admin_id, int user_id, const string& new_role, RoleResult& r, int& err) {
    vector<vector<string>> admins, target;
    err = 0;

    // STEP 1: saare ADMIN FOR UPDATE se lock, phir ginti
    if (!db_select_on(m, "SELECT user_id FROM users WHERE role = 'ADMIN' ORDER BY user_id FOR UPDATE", {}, admins, &err))
        return role_fail(err, r);
    bool actor_is_admin = false;
    for (auto& a : admins) {
        if (a.empty()) return role_fail(0, r);
        if (atoi(a[0].c_str()) == admin_id) actor_is_admin = true;
    }
    // lock milne tak is admin ka role kisi aur ne hata diya ho sakta hai: tab uska change bhi nahi chalega
    if (!actor_is_admin) return role_problem(r, 403, "not_admin");

    // STEP 2: jis user ka role badalna hai, uski row bhi lock
    if (!db_select_on(m, "SELECT role FROM users WHERE user_id = ? FOR UPDATE", {to_string(user_id)}, target, &err))
        return role_fail(err, r);
    if (target.empty() || target[0].empty()) return role_problem(r, 404, "no_user");
    const string current = target[0][0];
    r.from = current;
    if (current == new_role) return role_problem(r, 400, "same_role");
    if (current == "ADMIN" && new_role == "CUSTOMER" && admins.size() <= 1) return role_problem(r, 400, "last_admin");

    // STEP 3: badlo ("AND role = <purana>": 1 row badalni chahiye) aur audit row usi transaction me
    if (!exec_one(m, "UPDATE users SET role = ? WHERE user_id = ? AND role = ?", {new_role, to_string(user_id), current}, err))
        return role_fail(err, r);
    if (!exec_one(m, "INSERT INTO admin_actions (admin_id, action, details) VALUES (?, 'USER_ROLE', ?)",
                  {to_string(admin_id), "user " + to_string(user_id) + " role " + current + "->" + new_role}, err))
        return role_fail(err, r);
    return CO_OK;
}

static int role_attempt(MYSQL* m, int admin_id, int user_id, const string& new_role, RoleResult& r) {
    if (!db_begin(m)) {
        r.status = 500;
        r.code = "db_error";
        return CO_FAIL;
    }
    int err = 0;
    int code = role_steps(m, admin_id, user_id, new_role, r, err);
    if (code == CO_OK && mysql_commit(m)) {
        err = (int)mysql_errno(m);
        code = role_fail(err, r);
    }
    if (code == CO_OK) {
        r.status = 200;
        r.code = "";
        return code;
    }
    mysql_rollback(m);
    log_tag("ADMIN", "role change of user " + to_string(user_id) + " by user " + to_string(admin_id) + " ROLLBACK reason: " + r.code +
                     (err != 0 ? " (MySQL error " + to_string(err) + ")" : ""));
    return code;
}

RoleResult role_change(int admin_id, int user_id, const string& new_role) {
    RoleResult r;
    if (new_role != "ADMIN" && new_role != "CUSTOMER") { r.status = 400; r.code = "bad_request"; return r; }
    if (admin_id == user_id) { r.status = 400; r.code = "own_role"; return r; }     // apna role nahi, transaction se pehle hi

    int id = 0;
    MYSQL* m = get_conn(id);
    if (m == nullptr) return r;
    for (int attempt = 1; attempt <= CHECKOUT_RETRIES; attempt++) {
        r = RoleResult();
        int code = role_attempt(m, admin_id, user_id, new_role, r);
        if (code != CO_RETRY) break;
        if (attempt == CHECKOUT_RETRIES) {
            r.status = 503;
            r.code = "busy";
            log_tag("ADMIN", "role change of user " + to_string(user_id) + " gave up after " + to_string(attempt) + " attempts");
        } else {
            log_tag("ADMIN", "role change retry " + to_string(attempt + 1) + "/" + to_string(CHECKOUT_RETRIES));
            this_thread::sleep_for(chrono::milliseconds(50 * attempt));
        }
    }
    db_end(m);
    put_conn(id);
    return r;
}

void db_thread_start() {
    if (mysql_thread_init() != 0) log_tag("DB", "mysql_thread_init failed");
}

void db_thread_end() {
    mysql_thread_end();
}
