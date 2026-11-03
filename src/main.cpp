// main.cpp - program start; database, server aur workers start/stop karta hai
// Owner: Shoyam20
#include "common.h"
using namespace std;

int main() {
    log_msg("ServerSync starting...");

    if (!load_templates(TEMPLATE_DIR)) {    // templates ke bina koi page ban hi nahi sakta
        log_msg("Cannot start: templates are missing. Run the server from the project folder (it needs templates/).");
        return 1;
    }
    if (!init_db()) {                   // DB nahi chala to server shuru hi nahi karte
        log_msg("Cannot start without the database. Fix db.conf / MySQL and try again.");
        return 1;
    }
    if (!startServer()) {
        close_db();
        return 1;
    }
    start_logger();     // request_logs ka batch writer: workers se pehle, taaki pehli request ka log bhi aaye
    start_workers();
    start_cleanup();    // expired sessions ki safai (DB use karta hai)
    runServer();        // Ctrl+C aane tak chalta hai
    stop_workers();     // pehle workers (wo logs push karte hain), phir cleanup
    stop_cleanup();
    stop_logger();      // ab koi naya log nahi aayega: bacha hua sab DB me flush karke logger band
    close_db();         // sabse aakhir me DB connections (logger aur cleanup unhe use karte hain)
    stopServer();

    log_msg("ServerSync stopped.");
    return 0;
}
