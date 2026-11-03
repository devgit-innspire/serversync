// server.cpp - Winsock socket code: accept loop, recv/send, request parsing, static files (/static/...)
// Owner: Aradhana
#ifdef _WIN32
#include <winsock2.h>   // windows.h se pehle aana chahiye
#include <windows.h>
#else
#include <csignal>
#include <sys/time.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "common.h"
using namespace std;

// global socket, taaki Ctrl+C handler isse band kar sake
static SOCKET server = INVALID_SOCKET;

// listening socket band karo (dobara call karna safe hai)
static void closeServerSocket() {
    if (server != INVALID_SOCKET) {
        SOCKET s = server;
        server = INVALID_SOCKET;
        closesocket(s);   // Windows me close() nahi, closesocket() hota hai
    }
}

// Ctrl+C aane par socket band karo, accept() fail hoga aur loop khatam
#ifdef _WIN32
static BOOL WINAPI onCtrlC(DWORD) {
    closeServerSocket();
    return TRUE;
}
#else
// Linux: Ctrl+C = SIGINT, Render deploy/stop = SIGTERM
static void onCtrlC(int) {
    closeServerSocket();
}
#endif

// recv par timeout: Windows me DWORD milliseconds, Linux me timeval (seconds + microseconds)
static void set_recv_timeout(SOCKET client, int ms) {
#ifdef _WIN32
    DWORD t = ms;
#else
    timeval t;
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
#endif
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
}

// "123" jaise text ko number me badlo; galat ho to false
static bool to_number(const string& s, size_t& num) {
    if (s.empty() || s.size() > 9) return false;
    num = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        num = num * 10 + (c - '0');
    }
    return true;
}

// text ko lowercase karo (header naam ke liye)
static string to_lower(string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// aage-peeche ke space, tab, \r, \n hatao
string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// hex digit ka value (0-15), galat ho to -1
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL decode: "+" = space, "%20" = byte 0x20; galat %xx ya %00 ho to false
static bool url_decode(const string& in, string& out) {
    out = "";
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%') {
            if (i + 2 >= in.size()) return false;   // % ke baad do hex digit chahiye
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            char b = (char)(hi * 16 + lo);
            if (b == 0) return false;               // NUL byte nahi chalega
            out += b;
            i += 2;
        } else {
            out += c;
        }
    }
    return true;
}

// "cat=3&q=lap" ko params map me todo (naam aur value dono decode hote hain)
static bool parse_query(const string& query, map<string, string>& params) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == string::npos) amp = query.size();
        string pair = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty()) continue;

        size_t eq = pair.find('=');
        string rawName = (eq == string::npos) ? pair : pair.substr(0, eq);
        string rawVal = (eq == string::npos) ? "" : pair.substr(eq + 1);
        string name, val;
        if (!url_decode(rawName, name) || !url_decode(rawVal, val)) return false;
        if (!name.empty()) params[name] = val;
    }
    return true;
}

// request line padho: "GET /path HTTP/1.1"
static bool parse_request_line(const string& line, Request& req) {
    size_t a = line.find(' ');
    if (a == string::npos) return false;
    size_t b = line.find(' ', a + 1);
    if (b == string::npos) return false;

    req.method = line.substr(0, a);
    req.target = line.substr(a + 1, b - a - 1);     // jaisa aaya waisa: /products?cat=3
    req.version = line.substr(b + 1);

    // path aur ?query alag karo
    size_t q = req.target.find('?');
    req.path = req.target.substr(0, q);
    if (q != string::npos && !parse_query(req.target.substr(q + 1), req.params)) return false;

    if (req.method.empty()) return false;
    for (char c : req.method)
        if (c < 'A' || c > 'Z') return false;        // method sirf capital letters
    if (req.path.empty() || req.path[0] != '/') return false;
    for (char c : req.target)
        if (c <= 32 || c == 127) return false;       // control char / space nahi chalega
    if (req.version.compare(0, 5, "HTTP/") != 0) return false;
    if (req.version.find(' ') != string::npos) return false;
    return true;
}

// Cookie header "a=1; session_id=xyz; b=2" ko cookies map me todo
static void parse_cookies(const string& header, map<string, string>& cookies) {
    size_t pos = 0;
    while (pos < header.size()) {
        size_t semi = header.find(';', pos);
        if (semi == string::npos) semi = header.size();
        string pair = trim(header.substr(pos, semi - pos));
        pos = semi + 1;

        size_t eq = pair.find('=');
        if (eq == string::npos || eq == 0) continue;
        cookies[pair.substr(0, eq)] = pair.substr(eq + 1);
    }
}

// head = request line + headers (aakhri blank line ke bina); method, path, version, headers, cookies bharta hai
static bool parse_request(const string& head, Request& req) {
    size_t eol = head.find("\r\n");
    if (eol == string::npos) eol = head.size();
    if (!parse_request_line(head.substr(0, eol), req)) return false;

    // har header line: "Name: value"
    size_t pos = eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == string::npos) e = head.size();
        string line = head.substr(pos, e - pos);
        pos = e + 2;

        size_t colon = line.find(':');
        if (colon == string::npos || colon == 0) return false;
        string name = line.substr(0, colon);
        if (name.find_first_of(" \t") != string::npos) return false;
        req.headers[to_lower(name)] = trim(line.substr(colon + 1));
    }

    auto it = req.headers.find("cookie");
    if (it != req.headers.end()) parse_cookies(it->second, req.cookies);
    return true;
}

// read_request ke result codes (0 = theek, -1 = client chala gaya, baaki HTTP status)
const int READ_OK = 0;
const int READ_GONE = -1;

// recv se kuch bytes data me jodo; READ_OK, READ_GONE ya 408 (timeout) return karta hai
static int recv_more(SOCKET client, string& data) {
    char buf[4096];
    int n = recv(client, buf, sizeof(buf), 0);   // SO_RCVTIMEO ki wajah se zyada der nahi ruka rahega
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAETIMEDOUT) return 408;
        log_msg("recv failed: " + to_string(WSAGetLastError()));
        return READ_GONE;
    }
    if (n == 0) return READ_GONE;                // client ne connection band kar diya
    data.append(buf, n);
    return READ_OK;
}

// client se ek poori request padho; READ_OK, READ_GONE, 400 (galat/badi) ya 408 (timeout) return karta hai
static int read_request(SOCKET client, Request& req) {
    // TCP stream hai, isliye "\r\n\r\n" milne tak recv ka loop
    string raw;
    size_t end;
    while ((end = raw.find("\r\n\r\n")) == string::npos) {
        if (raw.size() > MAX_HEADER) return 400;     // headers bahut bade
        int r = recv_more(client, raw);
        if (r != READ_OK) return r;
    }
    if (end + 4 > MAX_HEADER) return 400;

    string body = raw.substr(end + 4);               // headers ke saath aaya body ka hissa
    if (!parse_request(raw.substr(0, end), req)) return 400;

    // body ki length Content-Length se pata chalti hai
    if (req.headers.count("transfer-encoding")) return 400;     // chunked abhi support nahi
    size_t len = 0;
    if (req.headers.count("content-length")) {
        if (!to_number(req.headers["content-length"], len)) return 400;
        if (len > MAX_BODY) return 400;
    }

    while (body.size() < len) {
        int r = recv_more(client, body);
        if (r != READ_OK) return r;
    }
    req.body = body.substr(0, len);

    // POST form (a=1&b=2) ko bhi query jaise hi decode karo
    if (req.method == "POST" &&
        to_lower(req.headers["content-type"]).compare(0, 33, "application/x-www-form-urlencoded") == 0) {
        if (!parse_query(req.body, req.form)) return 400;
    }
    return READ_OK;
}

// status code ka naam
static string reason(int status) {
    if (status == 200) return "OK";
    if (status == 302) return "Found";
    if (status == 400) return "Bad Request";
    if (status == 401) return "Unauthorized";
    if (status == 403) return "Forbidden";
    if (status == 404) return "Not Found";
    if (status == 405) return "Method Not Allowed";
    if (status == 409) return "Conflict";
    if (status == 408) return "Request Timeout";
    if (status == 500) return "Internal Server Error";
    if (status == 503) return "Service Unavailable";
    return "Unknown";
}

// poora data bhejo
static void sendAll(SOCKET client, const string& res) {
    // send kam bytes bhi bhej sakta hai, isliye loop
    size_t sent = 0;
    while (sent < res.size()) {
        int n = send(client, res.c_str() + sent, (int)(res.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            log_msg("send failed: " + to_string(WSAGetLastError()));
            break;
        }
        sent += n;
    }
}

// reply banao aur bhejo: status line, headers, blank line, body
// extra = optional extra header lines (default common.h me), har line "\r\n" par khatam (jaise "Retry-After: 2\r\n")
void send_response(SOCKET client, int status, const string& type, const string& body,
                   const string& extra) {
    // Content-Length body ke bytes ke barabar hona chahiye
    string res = "HTTP/1.1 " + to_string(status) + " " + reason(status) + "\r\n"
                 "Content-Type: " + type + "\r\n"
                 "Content-Length: " + to_string(body.size()) + "\r\n"
                 "Connection: close\r\n" + extra +
                 "\r\n" + body;
    sendAll(client, res);
}

// ---- static files: public/ ki files /static/<naam> par ----
// Saari files startup par ek baar memory me aa jaati hain (load_static_files), isliye request ke waqt disk nahi padhte.
// Isi wajah se path trick (../db.conf) bhi kaam nahi karta: hum disk par path banate hi nahi, sirf is map me naam dhundhte hain.
static map<string, StaticFile> static_files;

// extension se Content-Type; "" = unknown (aisi file load hi nahi hoti, to 404)
static string static_type(const string& name) {
    size_t dot = name.rfind('.');
    if (dot == string::npos) return "";
    string ext = name.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    return "";
}

// file ka naam safe hai? (sirf a-z A-Z 0-9 . _ - aur folder ke liye /)
static bool safe_name_chars(const string& name) {
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) return false;
    }
    return true;
}

// dir ki saari files (folders ke andar bhi) memory me; startup par ek baar. Return: kitni files load huin
int load_static_files(const string& dir) {
    static_files.clear();
    error_code ec;
    if (!filesystem::is_directory(dir, ec)) {
        log_msg("static folder '" + dir + "' not found: /static/... will return 404");
        return 0;
    }

    for (auto& entry : filesystem::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        string name = filesystem::relative(entry.path(), dir).generic_string();     // hamesha "/" se
        string type = static_type(name);
        if (type.empty() || !safe_name_chars(name) || name.find("..") != string::npos) {
            log_msg("static: skipped '" + name + "' (unknown type or unsafe name)");
            continue;
        }
        if (entry.file_size() > 2 * 1024 * 1024) {              // memory me rakhni hai: 2 MB se badi nahi
            log_msg("static: skipped '" + name + "' (bigger than 2 MB)");
            continue;
        }

        ifstream f(entry.path(), ios::binary);                  // binary: png ke bytes jaise ke waise
        StaticFile sf;
        sf.type = type;
        sf.data.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
        static_files[name] = sf;
    }
    log_msg("static files loaded: " + to_string(static_files.size()) + " from '" + dir + "'");
    return (int)static_files.size();
}

// "/static/" ke baad ka naam -> file. nullptr = 404.
// Mana hai: khali naam, ".." (upar ke folder), "\" (Windows folder), aur "/" se shuru hone wala naam
const StaticFile* find_static(const string& name) {
    if (name.empty()) return nullptr;
    if (name.find("..") != string::npos) return nullptr;
    if (name.find('\\') != string::npos) return nullptr;
    if (name[0] == '/') return nullptr;
    auto it = static_files.find(name);
    return it == static_files.end() ? nullptr : &it->second;
}

// GET /static/<naam>. Status return karta hai. Disk nahi padhta, memory se bhejta hai
static int serve_static(SOCKET client, const Request& req) {
    if (req.method != "GET") {
        send_response(client, 405, "text/plain; charset=utf-8", "Method Not Allowed", "Allow: GET\r\n");
        return 405;
    }
    const StaticFile* f = find_static(req.path.substr(8));      // "/static/" ke 8 characters hatao
    if (f == nullptr) {
        send_response(client, 404, "text/plain; charset=utf-8", "Not Found");
        return 404;
    }
    // browser ek ghante tak dobara nahi mangega
    send_response(client, 200, f->type, f->data, "Cache-Control: max-age=3600\r\n");
    return 200;
}

// ek client ko handle karo: request padho, route karo, band karo. Return: kya jawab gaya (worker log/metrics banata hai).
// Har request ki console line sirf VERBOSE_LOG = 1 par; baaki sab /metrics aur request_logs table me hai
RequestInfo handleClient(const ClientJob& job) {
    SOCKET client = job.sock;
    RequestInfo info;
    Request req;
    req.client_ip = job.ip;
    int r = read_request(client, req);

    if (r == READ_OK) {
        // /static/... login/DB ki zaroorat nahi: seedha memory se (route() aur load_user se pehle)
        bool is_static = req.path.compare(0, 8, "/static/") == 0;
        info.status = is_static ? serve_static(client, req) : route(client, req);
        info.method = req.method;
        info.path = req.path;
        info.user_id = req.user_id;
        log_msg_verbose(req.method + " " + req.target + " -> " + to_string(info.status));
    } else if (r == 408) {
        send_response(client, 408, "text/html; charset=utf-8", render_error_page("408 Request Timeout"));
        info.status = 408;
        info.method = "-";              // request poori padhi hi nahi gayi
        info.path = "-";
        log_msg_verbose("timeout -> 408");
    } else if (r == 400) {
        send_response(client, 400, "text/html; charset=utf-8", render_error_page("400 Bad Request"));
        info.status = 400;
        info.method = req.method.empty() ? "-" : req.method;   // request line theek thi to method/path mil jaate hain
        info.path = req.path.empty() ? "-" : req.path;
        log_msg_verbose("bad request -> 400");
    } else {
        log_msg_verbose("client left early");   // client chala gaya, reply bhejne ka fayda nahi (status 0 = log nahi)
    }
    closesocket(client);   // is client ka connection band
    return info;
}

bool startServer() {
#ifdef _WIN32
    SetConsoleCtrlHandler(onCtrlC, TRUE);
#else
    signal(SIGINT, onCtrlC);
    signal(SIGTERM, onCtrlC);
    signal(SIGPIPE, SIG_IGN);   // client chala gaya to send() process ko maar na de (sirf error lautaye)
#endif
    load_static_files(STATIC_DIR);      // workers shuru hone se pehle: baad me map sirf padhi jaati hai (mutex nahi chahiye)

    // WSAStartup: koi bhi socket call se pehle zaroori, version 2.2 maango
#ifdef _WIN32
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        log_msg("WSAStartup failed: " + to_string(r));
        return false;
    }
#endif

    // socket: OS se endpoint maango (AF_INET = IPv4, SOCK_STREAM = TCP)
    server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        log_msg("socket failed: " + to_string(WSAGetLastError()));
        WSACleanup();
        return false;
    }

    // SO_REUSEADDR: restart par port turant dobara use ho sake
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // bind: socket ko port se jodo (INADDR_ANY = saare network cards, htons = network byte order)
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg("bind failed: " + to_string(WSAGetLastError()));
        closeServerSocket();
        WSACleanup();
        return false;
    }

    // listen: socket ko "connections ka wait" mode me daalo (backlog queue OS rakhta hai)
    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        log_msg("listen failed: " + to_string(WSAGetLastError()));
        closeServerSocket();
        WSACleanup();
        return false;
    }

    log_msg("Listening on http://localhost:" + to_string(PORT));
    return true;
}

// queue full: wait mat karo, turant 503 do aur connection band
static void reject_client(SOCKET client) {
    send_response(client, 503, "text/html; charset=utf-8", render_error_page("Server busy, try again"),
                  "Retry-After: 2\r\n");

    // unread request ke saath closesocket karne par Windows RST bhejta hai aur client ka 503 kho sakta hai
    // isliye pehle FIN bhejo, request ek baar padh lo (sirf 100 ms tak ruko), phir band karo
    shutdown(client, SD_SEND);
    set_recv_timeout(client, 100);
    char buf[4096];
    recv(client, buf, sizeof(buf), 0);
    closesocket(client);
}

void runServer() {
    while (true) {
        // accept: client aane tak ruko, phir us client ka naya socket milta hai (aur uska address)
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        SOCKET client = accept(server, (sockaddr*)&from, &from_len);
        long long accepted = now_us();             // accept hote hi time note: queue_wait_ms / latency_ms yahin se
        if (client == INVALID_SOCKET) {
            if (server == INVALID_SOCKET) break;   // Ctrl+C se band hua, nikal jao
            log_msg("accept failed: " + to_string(WSAGetLastError()));
            continue;
        }
        // slow client poora server na roke, isliye recv par timeout
        set_recv_timeout(client, RECV_TIMEOUT_MS);

        // client ka IPv4 "a.b.c.d" (network byte order me 4 bytes). /metrics sirf 127.0.0.1 ke liye hai
        const unsigned char* b = (const unsigned char*)&from.sin_addr;
        ClientJob job;
        job.sock = client;
        job.accept_us = accepted;
        job.ip = to_string(b[0]) + "." + to_string(b[1]) + "." + to_string(b[2]) + "." + to_string(b[3]);

        // handle karna worker threads ka kaam hai; hum sirf queue me daalte hain
        if (!push_client(job)) {
            reject_client(client);
            log_msg("queue full -> 503 (rejected so far: " + to_string(metric_rejected()) + ")");
        }
    }
}

void stopServer() {
    closeServerSocket();
    // WSACleanup: WSAStartup ka jodi, Winsock ka kaam khatam
    WSACleanup();
}
