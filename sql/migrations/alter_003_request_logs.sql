-- alter_003_request_logs.sql
-- Adds the request_logs table to an EXISTING ecommerce_db. Nothing is dropped or changed; all your data stays.
-- Safe to run more than once (CREATE TABLE IF NOT EXISTS).
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_003_request_logs.sql
-- A fresh database made from ecommerce_db.sql already has this table.
-- Needs MySQL 8.0.16+ (CHECK constraints). Without this table the server still works; the logger thread
-- just reports that it cannot save the logs and counts them as dropped.

USE ecommerce_db;

CREATE TABLE IF NOT EXISTS request_logs (
    log_id        BIGINT AUTO_INCREMENT PRIMARY KEY,
    method        VARCHAR(10)   NOT NULL,
    path          VARCHAR(255)  NOT NULL,       -- without the query string
    status        SMALLINT      NOT NULL,
    latency_ms    DECIMAL(10,3) NOT NULL,       -- accept -> reply sent
    queue_wait_ms DECIMAL(10,3) NOT NULL,       -- accept -> a worker picked it up
    worker_id     INT           NOT NULL,
    user_id       INT           NULL,           -- NULL = not logged in
    created_at    TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP,   -- time of the request
    INDEX idx_request_logs_created (created_at),
    CONSTRAINT chk_request_logs_status CHECK (status BETWEEN 100 AND 599),
    CONSTRAINT chk_request_logs_times CHECK (latency_ms >= 0 AND queue_wait_ms >= 0)
) ENGINE=InnoDB;
