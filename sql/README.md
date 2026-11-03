# Database setup

`ecommerce_db.sql` is the only file a new setup needs. It creates the whole current schema from scratch
(15 tables with every CHECK, UNIQUE, generated column, index and foreign key) and loads the seed data:
15 categories, 52 products and the 36 Indian states / union territories. It creates no users and no orders.

Needs MySQL 8.0.16 or newer (older versions ignore CHECK constraints).

## 1. Import

Open a terminal in the project folder and run (it asks for your MySQL password):

```
"C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p --default-character-set=utf8mb4 < sql\ecommerce_db.sql
```

In Command Prompt that is all one line. In PowerShell the `<` redirect does not work, so use:

```
cmd /c '"C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p --default-character-set=utf8mb4 < sql\ecommerce_db.sql'
```

Always keep `--default-character-set=utf8mb4`: without it the client may use another character set and non-English text gets garbled.

**Warning:** the file starts with `DROP DATABASE IF EXISTS ecommerce_db`. Running it again deletes everything in that
database (users, orders, addresses) and rebuilds it. Do not run it on a database whose data you want to keep.

Check it worked:

```
"C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p -e "SELECT (SELECT COUNT(*) FROM categories) AS categories, (SELECT COUNT(*) FROM products) AS products, (SELECT COUNT(*) FROM states) AS states" ecommerce_db
```

It should print 15, 52 and 36.

Then copy `db.conf.example` to `db.conf` and put your MySQL login in it (`db.conf` is never committed).

## 2. Make an admin

Signup always creates a CUSTOMER. Sign up on the site first, then promote that account (change the email):

```
"C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe" -u root -p --default-character-set=utf8mb4 -e "UPDATE users SET role = 'ADMIN' WHERE email = 'you@example.com'" ecommerce_db
```

The role is read on every request, so it works at once (no new login needed); the "Admin" link then appears in the menu and
`/admin` opens. Later, an admin can change other users' roles from Admin > Users.

## 3. demo_data.sql

There is **no** `demo_data.sql` in this project. The sample data (categories, products, states) is already inside
`ecommerce_db.sql`, so one import gives a working shop. If a separate demo file with sample users or orders is added later,
it must be imported after `ecommerce_db.sql`, never before.

## Other files

- `migrations/` - old `alter_00X` scripts, only for old databases (see the README there).
