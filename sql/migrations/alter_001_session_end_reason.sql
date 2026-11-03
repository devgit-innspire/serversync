-- alter_001_session_end_reason.sql
-- Updates an EXISTING ecommerce_db (keeps all data). Run it once:
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_001_session_end_reason.sql
-- A fresh database made from ecommerce_db.sql already has all of this, do not run it there.
-- Needs MySQL 8.0.16+ for the CHECK constraint.

USE ecommerce_db;

ALTER TABLE user_sessions
    ADD COLUMN end_reason VARCHAR(10) NULL AFTER is_active,
    ADD CONSTRAINT chk_sessions_end_reason CHECK (end_reason IN ('LOGOUT', 'EXPIRED')),
    ADD INDEX idx_sessions_active_expires (is_active, expires_at);

-- Optional: fill in the reason for sessions that ended before this column existed.
-- Before this change only a logout set is_active = 0, so these were all logouts.
UPDATE user_sessions
SET end_reason = 'LOGOUT'
WHERE is_active = 0 AND end_reason IS NULL;
