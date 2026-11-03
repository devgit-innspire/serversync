# MySQL 8 for Render (private service). On the FIRST start with an empty disk the official image runs every file in
# /docker-entrypoint-initdb.d, so the schema + seed data load once; later restarts keep the data on the disk.
FROM mysql:8.0
COPY sql/ecommerce_db.sql /docker-entrypoint-initdb.d/
# Data in a sub-folder: a fresh Render disk has lost+found in its root, and MySQL refuses a non-empty datadir.
CMD ["mysqld", "--datadir=/var/lib/mysql/data", "--character-set-server=utf8mb4"]
