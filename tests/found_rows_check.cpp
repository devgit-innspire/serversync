// found_rows_check.cpp - used by test_session_sql.py: what does a prepared UPDATE report as "affected rows"?
// usage: found_rows_check <0|1> <session_id>     (1 = connect with CLIENT_FOUND_ROWS, like init_db does)
// Connects to the throwaway test server (127.0.0.1:3399, root, no password). Never uses db.conf.
#include <winsock2.h>
#include <mysql.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
using namespace std;

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    unsigned long flags = (atoi(argv[1]) == 1) ? CLIENT_FOUND_ROWS : 0;

    MYSQL* m = mysql_init(nullptr);
    if (!mysql_real_connect(m, "127.0.0.1", "root", "", "ecommerce_db", 3399, nullptr, flags)) {
        cerr << "connect failed: " << mysql_error(m) << endl;
        return 1;
    }

    // "no-op" update: writes the same value it already has, so nothing really changes
    const char* sql = "UPDATE user_sessions SET last_activity = last_activity WHERE session_id = ?";
    MYSQL_STMT* st = mysql_stmt_init(m);
    mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql));

    MYSQL_BIND b;
    memset(&b, 0, sizeof(b));
    unsigned long len = (unsigned long)strlen(argv[2]);
    b.buffer_type = MYSQL_TYPE_STRING;
    b.buffer = argv[2];
    b.buffer_length = len;
    b.length = &len;
    mysql_stmt_bind_param(st, &b);

    if (mysql_stmt_execute(st) != 0) {
        cerr << "execute failed: " << mysql_stmt_error(st) << endl;
        return 1;
    }
    cout << mysql_stmt_affected_rows(st) << endl;
    mysql_stmt_close(st);
    mysql_close(m);
    return 0;
}
