// addresses_check.cpp - runs the REAL address_* and do_checkout() functions from src/db.cpp against the throwaway test MySQL
// (127.0.0.1:3399, root, no password). src/db.cpp is #included so the pool arrays are reachable: we fill the pool
// with our own connections, so init_db() and db.conf are never used.
// Needs: users 1 and 2, and an ACTIVE cart with items for user 1 (tests/test_addresses.py makes them). Prints "KEY value" lines.
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
string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}
void log_verbose(const string& tag, const string& msg) { log_tag(tag, msg); }
void metric_checkout(bool) {}

static Address good(const string& name) {
    Address a;
    a.first_name = name; a.last_name = "Kumar"; a.phone = "9876543210"; a.line1 = "12 MG Road"; a.city = "Dehradun";
    a.state_id = "26"; a.pincode = "248001"; a.type = "HOME";
    return a;
}

static void show(const string& key, const string& value) { cout << key << " " << value << endl; }

int main() {
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

    // ---- validation (no DB) ----
    Address a = good("  Asha ");
    show("CHECK_OK", "[" + address_check(a) + "] first=[" + a.first_name + "]");     // trimmed
    a = good("A"); a.phone = "98765"; show("CHECK_PHONE", address_check(a));
    a = good("A"); a.phone = "98765abcde"; show("CHECK_PHONE_LETTERS", address_check(a));
    a = good("A"); a.pincode = "012345"; show("CHECK_PIN_ZERO", address_check(a));
    a = good("A"); a.pincode = "12345"; show("CHECK_PIN_SHORT", address_check(a));
    a = good("A"); a.type = "CAFE"; show("CHECK_TYPE", address_check(a));
    a = good("A"); a.first_name = ""; show("CHECK_NAME", address_check(a));
    a = good("A"); a.state_id = ""; show("CHECK_STATE", address_check(a));
    a = good("A"); a.line1 = string(151, 'x'); show("CHECK_LONG", address_check(a));

    // ---- save / default / limit / ownership ----
    Address a1 = good("First"), a2 = good("Second");
    address_check(a1); address_check(a2);
    AddressResult r1 = address_save(1, a1, true);
    AddressResult r2 = address_save(1, a2, true);
    show("SAVE1", to_string(r1.status) + " id=" + to_string(r1.address_id));
    show("SAVE2", to_string(r2.status) + " id=" + to_string(r2.address_id));
    Address bad = good("Bad"); bad.state_id = "999";
    show("SAVE_BAD_STATE", to_string(address_save(1, bad, true).status));
    show("DEFAULT2", to_string(address_set_default(1, r2.address_id).status));
    show("DEFAULT_OTHER_USER", to_string(address_set_default(2, r1.address_id).status));
    Address e = good("Edited"); e.id = to_string(r1.address_id); e.city = "Mumbai";
    show("EDIT_OTHER_USER", to_string(address_save(2, e, false).status));
    show("EDIT_OWN", to_string(address_save(1, e, false).status));
    int last = 0;
    for (int i = 0; i < 12; i++) {
        AddressResult r = address_save(1, good("Fill" + to_string(i)), true);
        if (r.status == 200) last = r.address_id;
        else { show("LIMIT_STATUS", to_string(r.status) + " " + r.message); break; }
    }
    show("DELETE_FOREIGN", to_string(address_delete(2, last).status));
    show("DELETE_OWN", to_string(address_delete(1, last).status));
    show("DELETE_AGAIN", to_string(address_delete(1, last).status));

    // ---- checkout ----
    show("CO_MISSING", to_string(do_checkout(1, r1.address_id + 1000).status));       // aisa address hai hi nahi
    show("CO_DELETED", to_string(do_checkout(1, last).status));
    AddressResult other = address_save(2, good("Other"), true);
    show("CO_OTHER_USERS", to_string(do_checkout(1, other.address_id).status));
    CheckoutResult ok = do_checkout(1, r1.address_id);
    show("CO_OK", to_string(ok.status) + " order=" + to_string(ok.order_id) + " " + ok.message);
    show("IDS", to_string(r1.address_id) + " " + to_string(ok.order_id));
    return 0;
}
