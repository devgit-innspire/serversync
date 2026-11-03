-- alter_008_order_tracking.sql   (REQUIRED for auto shipment tracking on an EXISTING database)
-- 1) orders.status may now be PLACED, PACKED, SHIPPED, OUT_FOR_DELIVERY, DELIVERED, CANCELLED
--    (PENDING / CONFIRMED were never set by the server and are removed).
-- 2) orders.next_step_at: when the delivery thread moves the order to its next status (NULL = no automatic step).
-- 3) new table order_status_history: one row for every status change, with who made it.
-- 4) one PLACED row is added for every existing order (at its created_at).
-- Keeps all data. Run it ONCE (ADD COLUMN fails the second time):
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_008_order_tracking.sql
-- A fresh database made from ecommerce_db.sql already has all of this, do not run it there.
-- Before running, this must return 0 (otherwise the new CHECK cannot be added):
--     SELECT COUNT(*) FROM orders WHERE status IN ('PENDING', 'CONFIRMED');
-- Orders that already exist keep next_step_at = NULL, so they are NOT moved automatically (the admin can still move them).
-- Needs MySQL 8.0.16+ (CHECK constraints).

USE ecommerce_db;

ALTER TABLE orders DROP CHECK chk_orders_status;
ALTER TABLE orders ADD CONSTRAINT chk_orders_status
    CHECK (status IN ('PLACED', 'PACKED', 'SHIPPED', 'OUT_FOR_DELIVERY', 'DELIVERED', 'CANCELLED'));

ALTER TABLE orders ADD COLUMN next_step_at DATETIME NULL;
CREATE INDEX idx_orders_next_step ON orders (next_step_at);

CREATE TABLE IF NOT EXISTS order_status_history (
    history_id BIGINT AUTO_INCREMENT PRIMARY KEY,
    order_id   INT NOT NULL,
    status     VARCHAR(20) NOT NULL,
    changed_at TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP,
    changed_by VARCHAR(10) NOT NULL,
    FOREIGN KEY (order_id) REFERENCES orders(order_id),
    INDEX idx_order_history_order (order_id, history_id),
    CONSTRAINT chk_history_status CHECK (status IN ('PLACED', 'PACKED', 'SHIPPED', 'OUT_FOR_DELIVERY', 'DELIVERED', 'CANCELLED')),
    CONSTRAINT chk_history_by CHECK (changed_by IN ('SYSTEM', 'ADMIN', 'CUSTOMER'))
) ENGINE=InnoDB;

-- one PLACED row per existing order (only for orders that have no history yet)
INSERT INTO order_status_history (order_id, status, changed_at, changed_by)
SELECT o.order_id, 'PLACED', o.created_at, 'CUSTOMER'
FROM orders o
WHERE NOT EXISTS (SELECT 1 FROM order_status_history h WHERE h.order_id = o.order_id);
