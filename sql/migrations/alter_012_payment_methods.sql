-- alter_012_payment_methods.sql   (REQUIRED for UPI / Card / Net Banking payments on an EXISTING database)
-- Adds payments.pay_detail and payments.txn_ref, makes method NOT NULL and allows only COD, UPI, CARD, NETBANKING.
-- Existing rows are all 'COD' and keep working; their new columns stay NULL.
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_012_payment_methods.sql
-- A fresh database made from ecommerce_db.sql already has all of this, do not run it there.
-- Run it ONCE (a second run stops with "duplicate column name", nothing breaks).
-- Needs MySQL 8.0.16+ (CHECK constraints).

USE ecommerce_db;

ALTER TABLE payments
    MODIFY COLUMN method VARCHAR(30) NOT NULL DEFAULT 'COD',
    ADD COLUMN pay_detail VARCHAR(80) NULL AFTER method,
    ADD COLUMN txn_ref VARCHAR(30) NULL AFTER pay_detail,
    ADD CONSTRAINT chk_payments_method CHECK (method IN ('COD', 'UPI', 'CARD', 'NETBANKING'));
