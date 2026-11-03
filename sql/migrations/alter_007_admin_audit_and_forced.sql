-- alter_007_admin_audit_and_forced.sql   (REQUIRED for admin panel phase A4 on an EXISTING database)
-- 1) user_sessions.end_reason may now also be 'FORCED' (an admin ended the session).
-- 2) new table admin_actions (audit trail of admin changes).
-- Keeps all data.
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_007_admin_audit_and_forced.sql
-- A fresh database made from ecommerce_db.sql already has both, do not run it there.
-- Safe to run twice: the CHECK is replaced by the same list and the table is only created when missing.
-- Needs MySQL 8.0.16+ (CHECK constraints).

USE ecommerce_db;

ALTER TABLE user_sessions DROP CHECK chk_sessions_end_reason;
ALTER TABLE user_sessions ADD CONSTRAINT chk_sessions_end_reason
    CHECK (end_reason IN ('LOGOUT', 'EXPIRED', 'FORCED'));

CREATE TABLE IF NOT EXISTS admin_actions (
    action_id  BIGINT AUTO_INCREMENT PRIMARY KEY,
    admin_id   INT NOT NULL,
    action     VARCHAR(50)  NOT NULL,
    details    VARCHAR(255) NOT NULL DEFAULT '',
    created_at TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (admin_id) REFERENCES users(user_id),
    INDEX idx_admin_actions_created (created_at)
) ENGINE=InnoDB;
