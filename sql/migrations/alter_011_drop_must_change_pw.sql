-- alter_011_drop_must_change_pw.sql   (only for a database where alter_010 was run)
-- The "forced password change" feature was removed, so the users.must_change_pw column added by alter_010 is not used any more.
-- Nothing else changes: passwords stay as they are, and the password rules still apply to new passwords (signup, /account/password).
--     mysql -u <user> -p --default-character-set=utf8mb4 ecommerce_db < sql\migrations\alter_011_drop_must_change_pw.sql
-- A fresh database made from ecommerce_db.sql never had this column, do not run it there.
-- Run it only once (a second run stops with "Can't DROP ... check that column/key exists", nothing breaks).
-- If you never ran alter_010, skip both alter_010 and this file.

USE ecommerce_db;

ALTER TABLE users DROP COLUMN must_change_pw;
