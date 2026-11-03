// static_check.cpp - tests the static file code of src/server.cpp (load_static_files, find_static, serve_static)
// over a REAL loopback socket, without a database, a worker pool or db.conf. server.cpp is #included so its static
// functions are reachable; what it needs from other files is stubbed below.
// usage: static_check <folder>          (the folder to load, like "public")
#include "../src/server.cpp"
#include <iostream>

// ---- stubs for what server.cpp expects from threads.cpp / pages.cpp ----
void log_tag(const string& tag, const string& msg) { cout << "[" << tag << "] " << msg << endl; }
void log_msg(const string& msg) { log_tag("TEST", msg); }
void log_msg_verbose(const string&) {}
long long now_us() { return 0; }
bool push_client(const ClientJob&) { return true; }
long long metric_rejected() { return 0; }
int route(SOCKET, Request&) { return 200; }
string render_error_page(const string& message) { return message; }

// connected socket pair on 127.0.0.1: server side `a`, client side `c`
static bool make_pair(SOCKET& a, SOCKET& c) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) return false;
    int len = sizeof(addr);
    getsockname(ls, (sockaddr*)&addr, &len);
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(c, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    a = accept(ls, nullptr, nullptr);
    closesocket(ls);
    return a != INVALID_SOCKET;
}

// ek GET /static/<name> ko serve_static se guzaro aur socket par jo bytes gaye wo wapas do
static string fetch(const string& path, const string& method = "GET") {
    SOCKET a, c;
    if (!make_pair(a, c)) return "";
    Request req;
    req.method = method;
    req.path = path;
    serve_static(a, req);
    closesocket(a);
    string raw;
    char buf[4096];
    while (true) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);
    }
    closesocket(c);
    return raw;
}

// checksum of the body bytes (so binary files can be compared)
static unsigned long long checksum(const string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    string dir = argc > 1 ? argv[1] : "public";

    int loaded = load_static_files(dir);        // (its log lines print first, then the count on a line of its own)
    cout << "COUNT " << loaded << endl;

    const char* names[] = {"style.css", "favicon.svg", "logo.png", "app.js", "sub/deep.css", "UPPER.CSS",
                           "notes.txt", "archive.zip", "bad name.css", "big.css", "missing.css", "sub", "sub/",
                           "../db.conf", "..\\db.conf", "sub/../../db.conf", "/etc/passwd", "/style.css", "..",
                           "%2e%2e/db.conf", "sub\\deep.css", "", "style.css?x=1", "a..b.css"};
    for (const char* n : names) {
        const StaticFile* f = find_static(n);
        cout << "FIND [" << n << "] " << (f ? "found " + f->type + " " + to_string(f->data.size()) : "null") << endl;
    }

    // over a real socket: the exact bytes a browser receives
    for (const char* n : {"style.css", "favicon.svg", "logo.png", "app.js", "sub/deep.css", "../db.conf", "nope.css", "/x", ""}) {
        string raw = fetch(string("/static/") + n);
        size_t split = raw.find("\r\n\r\n");
        string head = split == string::npos ? raw : raw.substr(0, split);
        string body = split == string::npos ? "" : raw.substr(split + 4);
        size_t eol = head.find("\r\n");
        cout << "HTTP [" << n << "] " << head.substr(0, eol) << " | body " << body.size() << " sum " << checksum(body) << endl;
        for (const char* h : {"Content-Type", "Content-Length", "Cache-Control", "Connection", "Allow"}) {
            size_t p = head.find(string("\r\n") + h + ": ");
            if (p != string::npos) {
                size_t e = head.find("\r\n", p + 2);
                cout << "HDR [" << n << "] " << head.substr(p + 2, e - p - 2) << endl;
            }
        }
    }
    string raw = fetch("/static/style.css", "POST");
    cout << "POST " << raw.substr(0, raw.find("\r\n")) << endl;
    cout << "POSTHDR " << (raw.find("Allow: GET") != string::npos ? "Allow: GET" : "none") << endl;

    // what is on disk, for the test to compare with
    for (const char* n : {"style.css", "logo.png", "sub/deep.css"}) {
        ifstream f(dir + "/" + n, ios::binary);
        string data((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
        cout << "DISK [" << n << "] " << data.size() << " sum " << checksum(data) << endl;
    }
    WSACleanup();
    return 0;
}
