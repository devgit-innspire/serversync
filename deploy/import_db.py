# Loads sql/ecommerce_db.sql into a remote MySQL 8 (e.g. Aiven free) over TLS. Needs: py -m pip install pymysql
# Usage (PowerShell), from the project folder:
#   $env:DB_HOST="mysql-xxx.aivencloud.com"; $env:DB_PORT="12345"; $env:DB_USER="avnadmin"; $env:DB_PASSWORD="..."
#   py deploy\import_db.py
# WARNING: the SQL file starts with DROP DATABASE IF EXISTS ecommerce_db (wipes users/orders if run again).
import os
import pymysql

ca = os.environ.get("DB_SSL_CA")          # optional: path to Aiven's ca.pem for certificate checking
tls = {"ca": ca} if ca else {"check_hostname": False, "verify_mode": False}

conn = pymysql.connect(host=os.environ["DB_HOST"], port=int(os.environ.get("DB_PORT", "3306")),
                       user=os.environ["DB_USER"], password=os.environ["DB_PASSWORD"],
                       charset="utf8mb4", ssl=tls,
                       client_flag=pymysql.constants.CLIENT.MULTI_STATEMENTS)
here = os.path.dirname(os.path.abspath(__file__))
sql = open(os.path.join(here, "..", "sql", "ecommerce_db.sql"), encoding="utf-8").read()

with conn.cursor() as cur:
    cur.execute(sql)
    while cur.nextset():                  # run every statement in the file
        pass
    cur.execute("SELECT (SELECT COUNT(*) FROM ecommerce_db.categories), (SELECT COUNT(*) FROM ecommerce_db.products), "
                "(SELECT COUNT(*) FROM ecommerce_db.states)")
    print("categories, products, states =", cur.fetchone(), "(expected 15, 52, 36)")
conn.commit()
conn.close()
