// common.h - shared settings, structs aur function declarations (saare modules yahi use karte hain)
// Owner: Shoyam20
#pragma once
#ifdef _WIN32
#include <winsock2.h>   // SOCKET type ke liye; windows.h se pehle aana chahiye
typedef int socklen_t;  // accept() ka address length (Linux me socklen_t, Windows me int)
#else
// Linux (Render par Docker): wahi Winsock naam POSIX sockets par, taaki baaki code dono OS par same rahe
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
typedef int SOCKET;                       // Linux me socket ek file descriptor (int) hai
const SOCKET INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1;
const int SD_SEND = SHUT_WR;
const int WSAETIMEDOUT = EAGAIN;          // SO_RCVTIMEO khatam hone par Linux recv EAGAIN deta hai
inline int closesocket(SOCKET s) { return close(s); }
inline int WSAGetLastError() { return errno; }
inline int WSACleanup() { return 0; }     // Linux me Winsock jaisa start/cleanup nahi hota
#endif
#include <string>
#include <map>
#include <vector>
using namespace std;

struct MYSQL;   // MySQL ka type; poora mysql.h sirf db.cpp me chahiye

// ---- settings ----
const int PORT = 8080;
const int WORKERS = 4;                    // worker threads ki ginti
const int QUEUE_SIZE = 16;                // queue me max kitne clients wait kar sakte hain
const int DB_POOL = 4;                    // MySQL connections ki ginti (pool size)
const int MAX_HEADER = 8 * 1024;          // headers ka max size (8 KB)
const int MAX_BODY = 1024 * 1024;         // body ka max size (1 MB)
const int RECV_TIMEOUT_MS = 5000;         // slow client ko 5 sec se zyada wait nahi
const int SESSION_MINUTES = 60;           // login session kitne minute chalega
const int CLEANUP_SECONDS = 60;           // expired sessions band karne wala thread kitni der baad chalega
const int MAX_CART_QTY = 10;              // ek product ki cart me max quantity
const int PW_MIN_LEN = 8;                 // password ki min lambai (characters)
const int PW_MAX_LEN = 64;                // password ki max lambai (characters)
const int MAX_ADDRESSES = 10;            // ek user ke max active shipping addresses
const int CHECKOUT_RETRIES = 3;           // deadlock (1213) / lock wait timeout (1205) par transaction kitni baar try karna

// auto shipment tracking: delivery thread order ko PLACED -> PACKED -> SHIPPED -> OUT_FOR_DELIVERY -> DELIVERED le jata hai
const int AUTO_DELIVERY = 1;              // 1 = delivery thread chalu, 0 = band (admin hi status badlega)
const int DELIVERY_TICK_SECONDS = 5;      // delivery thread kitni der baad dekhta hai ki kaunse orders ka time ho gaya
// har step ka delay (seconds): pichhle status ke itni der baad order is status me aata hai. Har baar min..max me random.
// Demo ke chhote numbers; asli dukaan me ghanton/dinon me hote
const int DELAY_PACKED_MIN = 20,           DELAY_PACKED_MAX = 40;             // PLACED -> PACKED
const int DELAY_SHIPPED_MIN = 30,          DELAY_SHIPPED_MAX = 60;            // PACKED -> SHIPPED
const int DELAY_OUT_FOR_DELIVERY_MIN = 40, DELAY_OUT_FOR_DELIVERY_MAX = 80;   // SHIPPED -> OUT_FOR_DELIVERY
const int DELAY_DELIVERED_MIN = 20,        DELAY_DELIVERED_MAX = 40;          // OUT_FOR_DELIVERY -> DELIVERED

// folders (server isi folder se chalta hai jisme db.conf hai; teeno yahin se relative hain)
const string STATIC_DIR = "public";       // css / svg / png / js: startup par memory me load, /static/<file> se milti hain
const string TEMPLATE_DIR = "templates";  // *.html templates: startup par memory me load

// storefront
const int BUDGET_MAX = 1000;              // "Under ₹1,000" link aur home page ke "Budget picks" ki upar ki seema (rupaye)
const int LOW_STOCK = 5;                  // itna ya kam stock bache to card par narangi "Only N left"

// logging aur metrics
const int VERBOSE_LOG = 0;                // 0 = console par "conn taken/returned" aur har request ki line nahi (startup, error,
                                          // CHECKOUT, CLEANUP, LOGGER lines rehti hain); 1 = sab kuch
const int LOG_QUEUE_SIZE = 1000;          // request_logs ki memory queue: itne entries tak wait, phir naye DROP (gine jaate hain)
const int LOG_FLUSH_SECONDS = 1;          // logger thread har itne second me batch INSERT karta hai
const int LOG_BATCH_MAX = 200;            // ek multi-row INSERT me max rows
const int LATENCY_WINDOW = 1000;          // avg/p95 latency pichle itne requests par
const int RPS_SECONDS = 10;               // requests/sec pichle itne second par

// Sirf testing ke settings (normal chalane par dono 0 rakho):
const int CHECKOUT_DELAY_MS = 0;          // checkout me rows lock karne ke BAAD itne ms ruko: races dikhane ke liye
const int TEST_FAIL_CHECKOUT = 0;         // 1 = INSERT orders ke baad jaan-boojh kar error: ROLLBACK test karne ke liye
const int DEBUG_SQL = 0;                  // 1 = har query ka SQL aur har param (value + length) log karo. Password wali
                                          // (login/signup) aur user_sessions wali queries ki values '***' dikhti hain

// ---- structs ----
// ek HTTP request ke parts
struct Request {
    string method;                  // GET, POST ...
    string target;                  // /products?cat=3 (jaisa aaya)
    string path;                    // /products (query ke bina)
    map<string, string> params;     // query ke params, decoded: cat -> 3
    string version;                 // HTTP/1.1
    map<string, string> headers;    // naam lowercase me
    map<string, string> cookies;    // Cookie header se: session_id -> abc...
    string body;
    map<string, string> form;       // POST form ke fields, decoded: email -> a@b.com

    // is request ka user (route() ke shuru me load_user bharta hai); har request ki apni copy
    int user_id = 0;                // 0 = login nahi hai
    string user_name;
    string role;                    // "CUSTOMER" ya "ADMIN" (har request par DB se, to role badalna agli request se chalta hai)
    string session_id;             // current session (login hai tabhi)
    bool clear_cookie = false;      // session expire mila: is request ke reply me cookie hata do
    bool refresh_cookie = false;    // logged-in user: reply me cookie naye Max-Age ke saath dobara bhejo
    int cart_count = 0;             // logged-in user ke cart me total items (nav bar ke liye)
    string client_ip;               // client ka IPv4, jaise "127.0.0.1" (/metrics sirf local ke liye)
};

// public/ ki ek file, poori memory me (type = Content-Type)
struct StaticFile {
    string type;
    string data;
};

// queue me ek client: socket, accept hone ka time (queue_wait_ms aur latency_ms yahin se naapte hain), uska IP
struct ClientJob {
    SOCKET sock = INVALID_SOCKET;
    long long accept_us = 0;        // now_us() jab accept() ne return kiya
    string ip;
};

// handleClient ka nateeja: kis request ka kya jawab gaya (worker isse log/metrics banata hai)
struct RequestInfo {
    string method;
    string path;                    // query ke bina
    int status = 0;                 // 0 = koi jawab nahi gaya (client bina request bheje chala gaya)
    int user_id = 0;                // 0 = login nahi
};

// request_logs table ki ek row (memory queue me rehti hai, logger thread DB me daalta hai)
struct LogEntry {
    string method;
    string path;
    int status = 0;
    double latency_ms = 0;          // accept -> reply bheji gayi
    double queue_wait_ms = 0;       // accept -> worker ne uthaya
    int worker_id = 0;
    int user_id = 0;                // 0 = NULL
    long long created = 0;          // unix seconds (request ka asli time, insert ka nahi)
};

// /metrics ka snapshot (threads.cpp ka metrics_snapshot bharta hai)
struct Metrics {
    long long uptime_s = 0;
    long long total = 0;            // jawab diye gaye requests (503 reject bhi)
    long long c2xx = 0, c3xx = 0, c4xx = 0, c5xx = 0;
    long long rejected_503 = 0;     // queue full hone par
    long long timeouts_408 = 0;
    int busy_workers = 0;           // abhi request handle kar rahe workers
    int queue_length = 0;           // queue me abhi kitne wait kar rahe
    int free_db_conns = 0;          // pool me abhi kitni connections khali
    int latency_samples = 0;        // avg/p95 kitne requests par nikale
    double avg_ms = 0;
    double p95_ms = 0;
    double rps = 0;                 // requests/sec pichle RPS_SECONDS me
    long long checkout_commit = 0;
    long long checkout_rollback = 0;
    long long dropped_logs = 0;     // memory queue full ya DB insert fail hone se gire log entries
};

// checkout ka nateeja (db.cpp ka do_checkout bharta hai)
struct CheckoutResult {
    int status = 500;               // 200 = order ban gaya, 409 = cart khali / stock kam (message dekho),
                                    // 503 = deadlock/timeout baar baar, 500 = koi aur error
    int order_id = 0;               // status 200 par
    string total;                   // status 200 par, jaise "4497.00"
    string message = "Database error";   // user ko dikhane layak (galti hone par)
                                    // (status 404 = chuna hua address nahi mila / kisi aur ka / delete ho chuka)
};

// checkout par chuna hua payment (pages.cpp form jaanch ke bharta hai). Gateway simulated hai: asli paise nahi katte
struct PaymentInfo {
    string method = "COD";          // COD, UPI, CARD, NETBANKING
    string detail;                  // dikhane layak: UPI ID / "VISA **** 1111" / bank ka naam. Poora card number ya CVV kabhi nahi
    string txn_ref;                 // online payment ka transaction id (COD me "")
    bool declined = false;          // bank ne mana kiya (test card / test UPI): checkout ROLLBACK, order nahi banta
};

// ek shipping address ka form (sab text; state_id bhi text me, validation address_check karta hai)
struct Address {
    string id;                      // "" = naya
    string first_name, last_name, phone;
    string line1, line2, landmark;  // line2, landmark optional
    string city, state_id, pincode;
    string type = "HOME";           // HOME, WORK, OTHER
};

// address badalne ka nateeja
struct AddressResult {
    int status = 500;               // 200 ho gaya, 400 galat input / limit (message dikhao), 404 address nahi mila (ya kisi aur ka),
                                    // 503 deadlock/timeout, 500 koi aur error
    string message = "Database error";
    int address_id = 0;             // 200 par: naya / badla hua address
};

// password badalne ka nateeja (db.cpp ka password_change bharta hai)
struct PasswordResult {
    int status = 500;               // 200 badal gaya, 403 purana password galat, 404 user nahi mila, 503 deadlock/timeout, 500 koi aur error
    string message = "Database error";
    int ended = 0;                  // 200 par: kitne dusre sessions band hue
};

// ---- server.cpp ----
bool startServer();                 // WSAStartup, socket, bind, listen
void runServer();                   // accept loop: client accept karke queue me daalta hai
void stopServer();                  // socket band + WSACleanup
RequestInfo handleClient(const ClientJob& job);   // ek client: read, parse, route, send, close; kya hua wo return
string trim(const string& s);       // aage-peeche ke space, tab, \r, \n hatao
int load_static_files(const string& dir);                 // dir ki saari files memory me (startup par ek baar); kitni mili
const StaticFile* find_static(const string& name);        // "style.css" -> file, galat/unknown path = nullptr (404)
// reply bhejo; extra = extra header lines, har line "\r\n" par khatam (jaise "Retry-After: 2\r\n")
void send_response(SOCKET client, int status, const string& type, const string& body,
                   const string& extra = "");

// ---- pages.cpp ----
// password ke niyam: fail hue niyamon ke messages (khali = theek). name / email / old_pw se: password me naam ya email ka hissa
// nahi, aur badalte waqt purane jaisa nahi (old_pw "" = signup / login: yeh niyam nahi lagta). Password abhi plain text hi hai (hash nahi)
vector<string> check_password(const string& pw, const string& name, const string& email, const string& old_pw);
bool load_templates(const string& dir);                   // dir ki saari .html templates memory me (startup par); sab zaroori mili?
// template ka naam + values: {{key}} -> HTML-escaped value, {{{key}}} -> jaisa hai waisa (sirf hamare banaye HTML ke liye)
string render(const string& name, const map<string, string>& values);
string render_error_page(const string& message);          // poora error page (layout ke saath), server.cpp ke 400/408/503 ke liye
int route(SOCKET client, Request& req);   // user load karo, path dekh kar page bhejo; status code return karta hai
int close_expired_sessions();             // saare expired sessions band karo; kitne band hue (error par -1)

// ---- threads.cpp ----
long long now_us();                 // monotonic clock, microseconds (accept time / latency naapne ke liye)
bool push_client(const ClientJob& job);   // queue me daalo; queue full ho to turant false (wait nahi)
ClientJob pop_client();             // queue se nikalo (queue khali ho to ruko)
void start_workers();               // WORKERS threads shuru karo
void stop_workers();                // workers ko band karo aur join karo
void start_logger();                // request_logs ke liye background thread (har LOG_FLUSH_SECONDS me batch INSERT)
void stop_logger();                 // bacha hua sab DB me daalo (flush), phir thread band
void push_request_log(const LogEntry& e);   // memory queue me daalo; kabhi block nahi, full ho to drop + gino
long long metric_rejected();        // queue full 503: total aur 5xx me jodo, 503 count return
void metric_checkout(bool commit);  // checkout COMMIT / ROLLBACK ginti
Metrics metrics_snapshot();         // abhi ke saare numbers (thread-safe)
void log_verbose(const string& tag, const string& msg);   // sirf VERBOSE_LOG = 1 par chhapta hai
void log_msg_verbose(const string& msg);                  // log_msg, par sirf VERBOSE_LOG = 1 par
void start_cleanup();               // background thread: har CLEANUP_SECONDS me expired sessions band (delivery thread bhi isi ke saath)
void stop_cleanup();                // us thread ko turant jagao, band karo, join karo
void start_delivery();              // delivery thread: har DELIVERY_TICK_SECONDS me jin orders ka time ho gaya unhe ek step aage (AUTO_DELIVERY = 1 par)
void stop_delivery();               // us thread ko turant jagao, band karo, join karo
void log_msg(const string& msg);    // [time][THREAD n] msg, thread-safe
void log_tag(const string& tag, const string& msg);   // [time][tag] msg (jaise tag = "DB")
int thread_id();                    // is thread ka number (0 = main thread, -1 = cleanup, -2 = logger thread, -3 = delivery)
string thread_name();               // "THREAD n", "CLEANUP", "LOGGER", "DELIVERY" ya "SERVER" (logs ke liye)

// ---- db.cpp ----
bool init_db();                     // db.conf padho, DB_POOL connections kholo; fail ho to false
void close_db();                    // saare connections band
MYSQL* get_conn(int& id);           // free connection lo (na ho to ruko); id = connection number
void put_conn(int id);              // connection wapas pool me
// prepared statements: sql me ? lagao, user ka data args me do (sql ke saath kabhi jodna nahi)
// db_select: sirf SELECT. rows me har row ki values text me aati hain (NULL = ""). false = error.
// conn_id (optional): kaun sa pool connection use hua (sirf testing pages ke liye)
bool db_select(const string& sql, const vector<string>& args, vector<vector<string>>& rows,
               int* conn_id = nullptr);
// db_exec: sirf INSERT/UPDATE/DELETE. WHERE se match hui rows ki ginti return karta hai (value same rahi
// tab bhi gini jaati hai: CLIENT_FOUND_ROWS), error par -1.
// err_code (optional): error par MySQL error number (jaise 1062 = duplicate entry)
int db_exec(const string& sql, const vector<string>& args, int* err_code = nullptr);
// bina user input wali query: pehli row ki pehli value
bool db_scalar(const string& sql, string& out, int* conn_id = nullptr);
// *_on: pehle se li hui connection par (caller get_conn/put_conn karta hai). Ek request ki saari queries
// ek hi connection par chalane ke liye. Baaki sab db_select/db_exec jaisa
bool db_select_on(MYSQL* m, const string& sql, const vector<string>& args, vector<vector<string>>& rows,
                  int* err_code = nullptr);   // err_code: error par MySQL error number (jaise 1213 deadlock)
int db_exec_on(MYSQL* m, const string& sql, const vector<string>& args, int* err_code = nullptr);
// cart (sab diya hua connection m use karte hain; cart_id hamesha session ke user se aata hai)
int find_active_cart(MYSQL* m, int user_id);        // ACTIVE cart ka id, nahi mila/error: 0 (kuch banata nahi)
int get_active_cart(MYSQL* m, int user_id);         // ACTIVE cart, na ho to banata hai (race me 1062 sambhalta hai); error: 0
bool cart_add(MYSQL* m, int cart_id, const string& product_id, const string& qty);   // atomic add, MAX_CART_QTY cap
int cart_set_qty(MYSQL* m, int cart_id, const string& product_id, const string& qty); // qty "0" = hata do; match rows, error -1
int cart_remove(MYSQL* m, int cart_id, const string& product_id);                     // hati rows, error -1
bool cart_items(MYSQL* m, int cart_id, vector<vector<string>>& rows);   // product_id, name, price, qty, stock
int cart_count(MYSQL* m, int user_id);              // cart ke total items, error -1
// checkout: ek pooled connection par ek DB transaction (retry deadlock par). Cart session ke user ka hota hai.
// address_id: user ka chuna hua shipping address; transaction ke andar check hota hai ki usi user ka aur active hai (nahi to 404)
// pay: COD (default) ya online; online PAID usi transaction me hota hai, declined ho to poora ROLLBACK
CheckoutResult do_checkout(int user_id, int address_id, const PaymentInfo& pay = PaymentInfo());
// shipping addresses (sab session ke user_id se; kisi aur ka address kabhi nahi milta)
string address_check(Address& a);   // trim karke jaanch: "" = theek, warna user ko dikhane layak message
AddressResult address_save(int user_id, const Address& a, bool is_new);   // naya (max MAX_ADDRESSES) ya edit; ek transaction
AddressResult address_delete(int user_id, int address_id);                // is_active = 0 (DELETE kabhi nahi)
AddressResult address_set_default(int user_id, int address_id);           // purana default hatao + naya lagao: ek transaction

// Order ka status badalna (admin: ship / deliver / cancel; customer: sirf apna PLACED order cancel). Ek DB transaction.
struct OrderResult {
    int status = 500;               // 200 ho gaya, 400 raasta allowed nahi, 404 order nahi mila (ya kisi aur ka),
                                    // 409 order beech me kisi aur ne badal diya, 503 deadlock/timeout baar baar, 500 koi aur error
    string message = "Database error";
    string from;                    // order ka purana status (log ke liye)
};
// expected_from: jo status page par dikha tha ("" = check nahi). as_admin = false: sirf user_id ka PLACED ya PACKED order, sirf CANCELLED
OrderResult order_change(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from);
// order_change jaisa, par history me "changed_by" bhi likhta hai: "SYSTEM" (delivery thread, user_id 0, as_admin true), "ADMIN", "CUSTOMER"
OrderResult order_change_by(int order_id, const string& to, int user_id, bool as_admin, const string& expected_from,
                            const string& changed_by);
// ek step aage ya CANCELLED: PLACED->PACKED->SHIPPED->OUT_FOR_DELIVERY->DELIVERED, aur DELIVERED se pehle kabhi bhi CANCELLED
bool order_transition_ok(const string& from, const string& to);
string order_next_status(const string& status);                   // agla step ("" = DELIVERED/CANCELLED/unknown: aage kuch nahi)
int order_eta_seconds(const string& status);                      // next_step_at ke BAAD ke baaki steps ka average time (seconds)
int deliver_due_orders();           // jin orders ka next_step_at aa gaya unhe ek step aage (max 50). Kitne badle, error par -1

// User ka role badalna (admin panel). Ek DB transaction: apna role nahi, aakhri ADMIN kabhi demote nahi.
struct RoleResult {
    int status = 500;               // 200 ho gaya, 400 niyam ke khilaf, 403 ab admin nahi raha, 404 user nahi, 503 busy, 500 error
    string code = "db_error";       // 400/403/404 par admin flash ka code: own_role, last_admin, same_role, no_user, not_admin, bad_request
    string from;                    // purana role (log ke liye)
};
RoleResult role_change(int admin_id, int user_id, const string& new_role);
// password badlo: purana sahi ho tabhi, naya password + keep_session ke siva is user ke baaki sab
// sessions band (end_reason 'LOGOUT'): sab ek transaction me
PasswordResult password_change(int user_id, const string& current, const string& new_pw, const string& keep_session);
// admin ke har badlav ki ek row admin_actions me (transaction ke bahar wale badlavon ke liye)
void admin_audit(int admin_id, const string& action, const string& details);
// request_logs me ek saath kai rows (ek multi-row INSERT). Kitni judi wo return, error par -1
int db_insert_request_logs(const vector<LogEntry>& rows);
int db_free_connections();          // pool me abhi kitni connections khali (/metrics ke liye)
void db_thread_start();             // worker thread shuru hote hi (mysql_thread_init)
void db_thread_end();               // worker thread band hone se pehle (mysql_thread_end)
