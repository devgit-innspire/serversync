-- alter_002_order_status_values.sql   (OPTIONAL)
-- The server's checkout inserts status 'PLACED'. A database created earlier already allows it
-- (PLACED, SHIPPED, DELIVERED, CANCELLED), so you do NOT need this script to make checkout work.
-- Run it only if you also want PENDING and CONFIRMED to be allowed values:
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_002_order_status_values.sql
-- It keeps all data and every existing status (nothing is renamed). A fresh database made from
-- ecommerce_db.sql already has this, do not run it there. Needs MySQL 8.0.16+ (CHECK constraints).

USE ecommerce_db;

ALTER TABLE orders DROP CHECK chk_orders_status;
ALTER TABLE orders ADD CONSTRAINT chk_orders_status
    CHECK (status IN ('PLACED', 'PENDING', 'CONFIRMED', 'SHIPPED', 'DELIVERED', 'CANCELLED'));
