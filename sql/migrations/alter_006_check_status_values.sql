-- alter_006_check_status_values.sql   (OPTIONAL: only if the check below shows a value is missing)
-- Admin orders (phase A3) uses these values, so the CHECK constraints must allow them:
--     orders.status   : PLACED, SHIPPED, DELIVERED, CANCELLED
--     payments.status : PENDING, PAID, CANCELLED
-- 1) Look at your database first (read only, changes nothing):
--        mysql -u <user> -p ecommerce_db -e "SHOW CREATE TABLE orders\G SHOW CREATE TABLE payments\G"
--    or:  SELECT CONSTRAINT_NAME, CHECK_CLAUSE FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA = 'ecommerce_db';
-- 2) If chk_orders_status lists all four and chk_payments_status lists PENDING, PAID and CANCELLED: do NOT run this file.
--    (A database made from ecommerce_db.sql, or from the older versions of it, already allows them.)
-- 3) Otherwise run it. It keeps all data; it only replaces the two CHECK lists with the full lists:
--        mysql -u <user> -p ecommerce_db < sql\migrations\alter_006_check_status_values.sql
-- Needs MySQL 8.0.16+ (CHECK constraints). If a DROP CHECK says the name does not exist, use the name SHOW CREATE TABLE printed.

USE ecommerce_db;

ALTER TABLE orders DROP CHECK chk_orders_status;
ALTER TABLE orders ADD CONSTRAINT chk_orders_status
    CHECK (status IN ('PLACED', 'PENDING', 'CONFIRMED', 'SHIPPED', 'DELIVERED', 'CANCELLED'));

ALTER TABLE payments DROP CHECK chk_payments_status;
ALTER TABLE payments ADD CONSTRAINT chk_payments_status
    CHECK (status IN ('PENDING', 'PAID', 'REFUNDED', 'CANCELLED'));
