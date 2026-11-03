-- alter_010_password_policy.sql   (SKIP IT: the feature was removed, see alter_011)
-- This added users.must_change_pw for a "forced password change" feature that no longer exists. The password RULES themselves need no
-- database change (they are checked in the server code, not in a CHECK). If you already ran this file, run alter_011_drop_must_change_pw.sql
-- to remove the unused column; if you never ran it, skip both files.
-- Kept only so the numbering and history stay complete.
--     mysql -u <user> -p --default-character-set=utf8mb4 ecommerce_db < sql\migrations\alter_010_password_policy.sql
-- Run it only once (a second run stops with "Duplicate column name", nothing breaks).

USE ecommerce_db;

ALTER TABLE users
    ADD COLUMN must_change_pw BOOLEAN NOT NULL DEFAULT FALSE;
