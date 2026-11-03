-- alter_004_users_role.sql   (REQUIRED for the admin panel on an EXISTING database)
-- Adds users.role: 'CUSTOMER' (default) or 'ADMIN'. All existing users become CUSTOMER; no data is lost or changed.
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_004_users_role.sql
-- A fresh database made from ecommerce_db.sql already has this column, do not run it there.
-- Run it only once (a second run stops with a "Duplicate ..." error, nothing breaks).
-- Needs MySQL 8.0.16+ (CHECK constraints).

USE ecommerce_db;

ALTER TABLE users
    ADD COLUMN role VARCHAR(10) NOT NULL DEFAULT 'CUSTOMER',
    ADD CONSTRAINT chk_users_role CHECK (role IN ('CUSTOMER', 'ADMIN'));

-- Admins are made only here (SQL) or by another admin later. Example (change the email):
--     UPDATE users SET role = 'ADMIN' WHERE email = 'you@example.com';
