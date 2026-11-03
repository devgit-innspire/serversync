-- alter_005_products_is_active.sql   (REQUIRED for admin panel phase A2 on an EXISTING database)
-- Adds products.is_active: TRUE = shown in the store, FALSE = hidden (admin "Hide" button).
-- Every existing product stays visible (default TRUE). No data is lost or changed.
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_005_products_is_active.sql
-- A fresh database made from ecommerce_db.sql already has this column, do not run it there.
-- Run it only once (a second run stops with a "Duplicate column name" error, nothing breaks).

USE ecommerce_db;

ALTER TABLE products
    ADD COLUMN is_active BOOLEAN NOT NULL DEFAULT TRUE;
