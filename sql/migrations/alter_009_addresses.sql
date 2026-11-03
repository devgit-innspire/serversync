-- alter_009_addresses.sql   (REQUIRED for shipping addresses on an EXISTING database)
-- Adds the tables states (36 rows), addresses and order_addresses. Nothing existing is changed or deleted.
-- Orders placed before this change simply have no order_addresses row (the pages show "No address (old order)").
--     mysql -u <user> -p ecommerce_db < sql\migrations\alter_009_addresses.sql
-- A fresh database made from ecommerce_db.sql already has all of this, do not run it there.
-- Run it ONCE (a second run stops with "table already exists", nothing breaks).
-- Needs MySQL 8.0.16+ (CHECK constraints).

USE ecommerce_db;

-- 11. STATES
-- The 28 states and 8 union territories of India (36 rows). A lookup table: an address points to a state by id,
-- so a state name is stored once and cannot be misspelled in different ways.
CREATE TABLE states (
    state_id   INT AUTO_INCREMENT PRIMARY KEY,
    state_name VARCHAR(60) NOT NULL UNIQUE
) ENGINE=InnoDB;

INSERT INTO states (state_name) VALUES
    ('Andhra Pradesh'), ('Arunachal Pradesh'), ('Assam'), ('Bihar'), ('Chhattisgarh'), ('Goa'), ('Gujarat'),
    ('Haryana'), ('Himachal Pradesh'), ('Jharkhand'), ('Karnataka'), ('Kerala'), ('Madhya Pradesh'), ('Maharashtra'),
    ('Manipur'), ('Meghalaya'), ('Mizoram'), ('Nagaland'), ('Odisha'), ('Punjab'), ('Rajasthan'), ('Sikkim'),
    ('Tamil Nadu'), ('Telangana'), ('Tripura'), ('Uttar Pradesh'), ('Uttarakhand'), ('West Bengal'),
    ('Andaman and Nicobar Islands'), ('Chandigarh'), ('Dadra and Nagar Haveli and Daman and Diu'), ('Delhi'),
    ('Jammu and Kashmir'), ('Ladakh'), ('Lakshadweep'), ('Puducherry');

-- 12. ADDRESSES (shipping addresses saved by a user)
-- MULTIVALUED ATTRIBUTE: a user can have many addresses, so "address" cannot be one column of users.
-- A multivalued attribute becomes its OWN table, with a foreign key back to the owner (user_id).
-- COMPOSITE ATTRIBUTE: an address is made of parts (line1, city, state, pincode ...). A composite attribute
-- becomes separate columns, so each part can be checked, searched and filtered on its own.
-- is_active = 0 means "deleted by the user" (soft delete): the row stays, nothing is ever DELETEd.
CREATE TABLE addresses (
    address_id   INT AUTO_INCREMENT PRIMARY KEY,
    user_id      INT NOT NULL,
    first_name   VARCHAR(50)  NOT NULL,
    last_name    VARCHAR(50)  NOT NULL,
    phone        CHAR(10)     NOT NULL,
    line1        VARCHAR(150) NOT NULL,
    line2        VARCHAR(150) NULL,
    landmark     VARCHAR(100) NULL,
    city         VARCHAR(80)  NOT NULL,
    state_id     INT          NOT NULL,
    pincode      CHAR(6)      NOT NULL,
    address_type VARCHAR(10)  NOT NULL DEFAULT 'HOME',
    is_default   BOOLEAN      NOT NULL DEFAULT FALSE,
    is_active    BOOLEAN      NOT NULL DEFAULT TRUE,
    created_at   TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    -- Generated column: equals user_id while the address is the ACTIVE default, otherwise NULL.
    -- A UNIQUE index ignores NULLs, so a user can have many non-default addresses but only ONE default
    -- (same trick as one ACTIVE cart per user).
    default_user INT GENERATED ALWAYS AS (CASE WHEN is_default AND is_active THEN user_id ELSE NULL END) STORED,
    FOREIGN KEY (user_id)  REFERENCES users(user_id),
    FOREIGN KEY (state_id) REFERENCES states(state_id),
    CONSTRAINT uq_one_default_address UNIQUE (default_user),
    INDEX idx_addresses_user (user_id, is_active),
    -- REGEXP is allowed in a CHECK (it is deterministic). The server validates the same rules in C++ first,
    -- to show a clear message; these checks are the last line of defence.
    CONSTRAINT chk_addresses_phone   CHECK (phone REGEXP '^[0-9]{10}$'),
    CONSTRAINT chk_addresses_pincode CHECK (pincode REGEXP '^[1-9][0-9]{5}$'),
    CONSTRAINT chk_addresses_type    CHECK (address_type IN ('HOME', 'WORK', 'OTHER'))
) ENGINE=InnoDB;

-- 13. ORDER ADDRESSES (the address an order was shipped to)
-- SNAPSHOT, like order_items.price: checkout COPIES the address fields (and the state NAME as text) into this table.
-- There is deliberately NO foreign key to addresses or states: if the user later edits or deletes the address
-- (or a state is renamed), the old order must still show exactly where it was sent.
-- One row per order (order_id is the primary key), written in the same transaction as the order.
CREATE TABLE order_addresses (
    order_id     INT PRIMARY KEY,
    first_name   VARCHAR(50)  NOT NULL,
    last_name    VARCHAR(50)  NOT NULL,
    phone        CHAR(10)     NOT NULL,
    line1        VARCHAR(150) NOT NULL,
    line2        VARCHAR(150) NULL,
    landmark     VARCHAR(100) NULL,
    city         VARCHAR(80)  NOT NULL,
    state_name   VARCHAR(60)  NOT NULL,
    pincode      CHAR(6)      NOT NULL,
    address_type VARCHAR(10)  NOT NULL,
    FOREIGN KEY (order_id) REFERENCES orders(order_id)
) ENGINE=InnoDB;
