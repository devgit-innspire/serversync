# Deploying ServerSync for free (Aiven MySQL + Render + Vercel)

All three have free plans. The C++ server runs on **Render** (Linux, Docker), the database is a free **Aiven MySQL 8**,
and **Vercel** only forwards its URL to Render (Vercel cannot run a long-lived socket server).

## 1. Database: Aiven free MySQL

1. Sign up at https://aiven.io > **Create service** > **MySQL** > **Free plan** > create.
2. From the service's **Overview** copy Host, Port, User (`avnadmin`) and Password.
3. Load the schema + 52 products once, from the project folder (PowerShell):
   ```
   py -m pip install pymysql
   $env:DB_HOST="<host>"; $env:DB_PORT="<port>"; $env:DB_USER="avnadmin"; $env:DB_PASSWORD="<password>"
   py deploy\import_db.py
   ```
   It should print `(15, 52, 36)`.

## 2. Server: Render free web service

1. Render Dashboard > **New > Blueprint** > connect this GitHub repo (it reads `render.yaml`).
2. Fill in `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD` with the Aiven values (`DB_NAME` is already `ecommerce_db`).
3. Deploy. The URL looks like `https://serversync-xxxx.onrender.com`.

Free plan limits: the service sleeps after 15 minutes without visitors, and the first visit after that takes ~1 minute
to wake it up. The background threads (session cleanup, delivery tracking) also pause while it sleeps.

## 3. Vercel (proxy, optional)

1. Put your Render URL in `vercel/vercel.json`, commit, push.
2. Vercel > **Add New > Project** > import this repo > **Root Directory** = `vercel` > Deploy.

## 4. Make an admin

Sign up on the site, then (same PowerShell window as step 1.3):
```
py -c "import os,pymysql; c=pymysql.connect(host=os.environ['DB_HOST'],port=int(os.environ['DB_PORT']),user=os.environ['DB_USER'],password=os.environ['DB_PASSWORD'],database='ecommerce_db',ssl={'check_hostname':False,'verify_mode':False}); c.cursor().execute(\"UPDATE users SET role='ADMIN' WHERE email='you@example.com'\"); c.commit()"
```

## Local (Windows) is unchanged

`build.bat` + `db.conf` still work exactly as before.
