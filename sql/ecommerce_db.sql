-- ecommerce_db.sql - the WHOLE current schema plus seed data, in one file. New setups use only this file.
-- Import it as described in sql/README.md (use --default-character-set=utf8mb4: the data has the rupee sign and '&').
--
-- WARNING: it starts with DROP DATABASE IF EXISTS, so running it again wipes the database and rebuilds it from zero.
--
-- What it creates (15 tables):
--   users, user_sessions, categories, products, cart, cart_items, orders, order_status_history, order_items,
--   payments, request_logs, admin_actions, states, addresses, order_addresses
-- Seed data: 15 categories, 52 products, 36 states / union territories. No users, no orders (signup creates users).
--
-- Needs MySQL 8.0.16 or newer (CHECK constraints are ignored by older versions).

SET NAMES utf8mb4;

DROP DATABASE IF EXISTS ecommerce_db;
CREATE DATABASE ecommerce_db CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
USE ecommerce_db;

-- Every table uses InnoDB: it is the MySQL engine that supports transactions
-- (ACID), row-level locks and foreign keys. MyISAM supports none of these.
-- Every table inherits utf8mb4 from the database, so any text (names, addresses) is stored correctly.

-- 1. USERS
-- Stores information about registered customers.
CREATE TABLE users (
    user_id    INT AUTO_INCREMENT PRIMARY KEY,
    email      VARCHAR(100) NOT NULL UNIQUE,
    name       VARCHAR(100) NOT NULL,
    -- Stored exactly as typed (no hashing yet). The password RULES (length, upper/lower/digit/symbol ...) are NOT a CHECK here:
    -- the server's check_password applies them to typed passwords, and a hashed password would not look like a typed one.
    password   VARCHAR(255) NOT NULL,
    -- CUSTOMER = normal shopper (signup always creates this). ADMIN = can open /admin.
    -- Admins are made only with SQL (UPDATE users SET role = 'ADMIN' ...) or by another admin.
    role       VARCHAR(10) NOT NULL DEFAULT 'CUSTOMER',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT chk_users_role CHECK (role IN ('CUSTOMER', 'ADMIN'))
) ENGINE=InnoDB;

-- 2. USER SESSIONS
-- Stores every login session of every user.
-- One user can have multiple sessions.
-- Example:
-- Radhika -> Chrome  -> Session A123
-- Radhika -> Edge    -> Session B456
-- Shreya -> Chrome -> Session C789

CREATE TABLE user_sessions (
    session_id VARCHAR(64) PRIMARY KEY,
    user_id INT NOT NULL,
    login_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    logout_time TIMESTAMP NULL,
    last_activity TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    -- When the session stops being valid. The server sets it at login
    -- (e.g. NOW() + 1 day) and rejects the session after this time.
    expires_at TIMESTAMP NOT NULL,
    is_active BOOLEAN NOT NULL DEFAULT TRUE,
    -- Why the session ended: NULL while it is still active.
    -- LOGOUT = the user logged out, EXPIRED = expires_at passed (closed by a request or the cleanup thread),
    -- FORCED = an admin ended the session (Admin > Sessions, or "Log out everywhere" on Admin > Users).
    end_reason VARCHAR(10) NULL,
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    -- A CHECK passes when the value is NULL, so active sessions are fine.
    CONSTRAINT chk_sessions_end_reason CHECK (end_reason IN ('LOGOUT', 'EXPIRED', 'FORCED'))
) ENGINE=InnoDB;

-- Indexes for faster session lookup
CREATE INDEX idx_sessions_user
ON user_sessions(user_id);

CREATE INDEX idx_sessions_active
ON user_sessions(is_active);

CREATE INDEX idx_sessions_login
ON user_sessions(login_time);

CREATE INDEX idx_sessions_expires
ON user_sessions(expires_at);

-- For the cleanup job: WHERE is_active = 1 AND expires_at < NOW()
-- (the leftmost column is_active is compared with =, then expires_at is a range)
CREATE INDEX idx_sessions_active_expires
ON user_sessions(is_active, expires_at);

-- 3. CATEGORIES
-- Stores different product categories.
-- Example: Electronics, Audio, Accessories
CREATE TABLE categories (
    category_id INT AUTO_INCREMENT PRIMARY KEY,
    category_name VARCHAR(50) NOT NULL UNIQUE
) ENGINE=InnoDB;

-- INSERT CATEGORIES
INSERT INTO categories (category_name)
VALUES
('Electronics'),
('Accessories'),
('Audio'),
('Mobile Phones'),
('Laptops'),
('Computers'),
('Cameras'),
('Gaming'),
('Clothing'),
('Shoes'),
('Books'),
('Home & Kitchen'),
('Beauty & Personal Care'),
('Sports & Fitness'),
('Toys & Games');

-- 4. PRODUCTS 
-- Stores information about products available in the store. 
CREATE TABLE products (
    product_id   INT AUTO_INCREMENT PRIMARY KEY,
    name         VARCHAR(100)   NOT NULL,
    description  VARCHAR(255),
    price        DECIMAL(10,2)  NOT NULL,
    stock        INT            NOT NULL DEFAULT 0,
    -- FALSE = hidden from the store (lists, search, home, product page, cart/add) but the row stays:
    -- old orders point to it, so products are hidden, never deleted.
    is_active    BOOLEAN        NOT NULL DEFAULT TRUE,
    category_id INT NOT NULL,
    FOREIGN KEY (category_id) REFERENCES categories(category_id),
    -- The database itself refuses negative stock/price, even if two threads
    -- race or the C++ code has a bug.
    CONSTRAINT chk_products_price CHECK (price >= 0),
    CONSTRAINT chk_products_stock CHECK (stock >= 0)
) ENGINE=InnoDB;

-- SAMPLE PRODUCTS
INSERT INTO products(name, description, price, stock, category_id)
VALUES
-- 1. Electronics (category_id = 1)
('Smart LED Bulb', 'WiFi-enabled LED bulb with app control', 499.00, 40, 1),
('Portable SSD', '500GB portable solid state drive', 3999.00, 15, 1),
('Power Bank 20000mAh', 'Fast charging power bank with USB-C', 1799.00, 30, 1),
('Smart Plug', 'WiFi smart plug with energy monitoring', 799.00, 25, 1),

-- 2. Accessories (category_id = 2)
('USB-C Fast Charger', '30W fast charging wall adapter', 899.00, 45, 2),
('Magnetic Phone Holder', 'Adjustable magnetic mobile holder', 499.00, 35, 2),
('Laptop Backpack', 'Water-resistant backpack for laptops', 1499.00, 20, 2),
('Wireless Charging Pad', 'Qi-compatible wireless charging pad', 999.00, 25, 2),

-- 3. Audio (category_id = 3)
('TWS Wireless Earbuds', 'Bluetooth earbuds with charging case', 1499.00, 40, 3),
('Bluetooth Party Speaker', 'Portable wireless speaker with deep bass', 2499.00, 18, 3),
('ANC Headphones', 'Wireless headphones with noise cancellation', 3499.00, 15, 3),
('Neckband Earphones', 'Wireless neckband with long battery life', 999.00, 30, 3),

-- 4. Mobile Phones (category_id = 4)
('Smartphone X3', '128GB smartphone with high-resolution camera', 18999.00, 12, 4),
('5G Smartphone Y5', '5G smartphone with 256GB storage', 24999.00, 10, 4),
('Clear Phone Case', 'Transparent shockproof phone case', 299.00, 60, 4),
('Privacy Screen Protector', 'Privacy glass screen protector', 349.00, 50, 4),

-- 5. Laptops (category_id = 5)
('Student Laptop Pro', 'Laptop for study and everyday productivity', 45999.00, 8, 5),
('Laptop Cooling Stand', 'Adjustable laptop stand with cooling fan', 1299.00, 20, 5),
('Laptop Privacy Screen', 'Anti-glare privacy screen for laptops', 999.00, 15, 5),

-- 6. Computers (category_id = 6)
('Mechanical RGB Keyboard', 'Mechanical keyboard with RGB lighting', 2499.00, 20, 6),
('Wireless Gaming Mouse', 'Rechargeable mouse with adjustable DPI', 1599.00, 25, 6),
('Full HD Webcam', '1080p webcam for meetings and streaming', 1799.00, 15, 6),

-- 7. Cameras (category_id = 7)
('Action Camera', 'Compact camera for travel and outdoor recording', 7999.00, 8, 7),
('Flexible Camera Tripod', 'Portable adjustable tripod for cameras', 899.00, 20, 7),
('Ring Light with Stand', 'LED ring light for videos and online meetings', 1199.00, 25, 7),

-- 8. Gaming (category_id = 8)
('Wireless Game Controller', 'Wireless controller for PC gaming', 1999.00, 15, 8),
('Gaming Headset Pro', 'Over-ear gaming headset with microphone', 2299.00, 18, 8),
('Extended Gaming Mouse Pad', 'Large non-slip mouse pad for gaming desks', 699.00, 30, 8),
('Gaming Desk Light', 'RGB ambient light for gaming setups', 999.00, 20, 8),

-- 9. Clothing (category_id = 9)
('Oversized Graphic T-Shirt', 'Casual oversized cotton graphic T-shirt', 599.00, 40, 9),
('Wide-Leg Jeans', 'Relaxed-fit denim jeans for everyday wear', 1399.00, 25, 9),
('Cotton Co-ord Set', 'Comfortable matching casual clothing set', 1299.00, 20, 9),
('Lightweight Hoodie', 'Soft casual hoodie for everyday use', 1199.00, 22, 9),

-- 10. Shoes (category_id = 10)
('Chunky Sneakers', 'Casual chunky sole everyday sneakers', 1999.00, 20, 10),
('Running Shoes Pro', 'Lightweight cushioned running shoes', 2499.00, 18, 10),
('Slides Sandals', 'Comfortable lightweight casual slides', 499.00, 35, 10),

-- 11. Books (category_id = 11)
('Python Programming Guide', 'Beginner-friendly Python programming book', 599.00, 20, 11),
('Data Science Handbook', 'Introduction to data analysis and visualization', 799.00, 15, 11),
('AI and Machine Learning', 'Introduction to artificial intelligence concepts', 899.00, 12, 11),

-- 12. Home & Kitchen (category_id = 12)
('Digital Air Fryer', 'Compact air fryer for everyday cooking', 4999.00, 10, 12),
('Electric Coffee Frother', 'Handheld milk and coffee frother', 699.00, 25, 12),
('Rechargeable Table Lamp', 'Dimmable LED lamp for study desks', 799.00, 30, 12),
('Kitchen Storage Organizer', 'Multi-purpose kitchen storage rack', 999.00, 20, 12),

-- 13. Beauty & Personal Care (category_id = 13)
('Daily Sunscreen SPF 50', 'Lightweight sunscreen for daily use', 499.00, 35, 13),
('Hydrating Face Serum', 'Hydrating facial serum for skincare routines', 599.00, 25, 13),
('Electric Grooming Trimmer', 'Rechargeable trimmer for personal grooming', 1299.00, 18, 13),
('Lip Care Balm Set', 'Everyday lip care balm multipack', 299.00, 40, 13),

-- 14. Sports & Fitness (category_id = 14)
('Smart Fitness Band', 'Activity tracker with step counting', 1999.00, 15, 14),
('Resistance Band Set', 'Exercise resistance bands for home workouts', 499.00, 30, 14),
('Insulated Water Bottle', 'Reusable insulated stainless steel bottle', 799.00, 25, 14),

-- 15. Toys & Games (category_id = 15)
('STEM Building Kit', 'Educational construction kit for children', 999.00, 15, 15),
('Family Board Game', 'Strategy board game for family game nights', 699.00, 20, 15);


-- 5. CART
-- Stores shopping carts belonging to users.
-- A user can have multiple carts over time.
-- The database enforces that a user has only ONE cart with status = 'ACTIVE' at a time
-- (see active_user_id below).
-- Example:
-- Cart 1 -> CHECKED_OUT
-- Cart 2 -> CHECKED_OUT
-- Cart 3 -> ACTIVE
CREATE TABLE cart (
    cart_id  INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,
    status VARCHAR(20) NOT NULL DEFAULT 'ACTIVE',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    -- Generated column: equals user_id while the cart is ACTIVE, otherwise NULL.
    -- A UNIQUE index ignores NULLs, so any number of CHECKED_OUT carts are fine,
    -- but a second ACTIVE cart for the same user is rejected.
    active_user_id INT GENERATED ALWAYS AS (CASE WHEN status = 'ACTIVE' THEN user_id ELSE NULL END) STORED,
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    CONSTRAINT chk_cart_status CHECK (status IN ('ACTIVE', 'CHECKED_OUT')),
    CONSTRAINT uq_one_active_cart UNIQUE (active_user_id)
) ENGINE=InnoDB;

-- 6. CART ITEMS
-- Stores products added to a shopping cart.
-- One cart can contain many products.
CREATE TABLE cart_items (
    cart_item_id INT AUTO_INCREMENT PRIMARY KEY,
    cart_id      INT NOT NULL,
    product_id   INT NOT NULL,
    quantity     INT NOT NULL DEFAULT 1,
    FOREIGN KEY (cart_id)    REFERENCES cart(cart_id),
    FOREIGN KEY (product_id) REFERENCES products(product_id),
    -- Same product should appear only once
    -- in a particular cart.
    -- Quantity should be updated instead.
    UNIQUE(cart_id, product_id),
    CONSTRAINT chk_cart_items_quantity CHECK (quantity > 0)
) ENGINE=InnoDB;

-- 7. ORDERS
-- Stores orders created after checkout.
-- A cart is converted into an order during checkout.
-- The cart_id keeps track of which cart created the order.
CREATE TABLE orders (
    order_id    INT AUTO_INCREMENT PRIMARY KEY,
    user_id     INT NOT NULL,
    cart_id     INT NOT NULL,
    total       DECIMAL(10,2) NOT NULL,
    status      VARCHAR(20) NOT NULL DEFAULT 'PLACED',
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    -- When the server's delivery thread moves the order to its next status. NULL = no automatic step (final status).
    next_step_at DATETIME NULL,
    FOREIGN KEY (user_id) REFERENCES users(user_id),
    FOREIGN KEY (cart_id) REFERENCES cart(cart_id),
    UNIQUE(cart_id),
    CONSTRAINT chk_orders_total CHECK (total >= 0),
    -- The order status values. Checkout inserts PLACED (in one transaction); then PACKED, SHIPPED, OUT_FOR_DELIVERY, DELIVERED
    -- follow one step at a time (delivery thread or admin). CANCELLED is possible before DELIVERED.
    CONSTRAINT chk_orders_status CHECK (status IN ('PLACED', 'PACKED', 'SHIPPED', 'OUT_FOR_DELIVERY', 'DELIVERED', 'CANCELLED')),
    INDEX idx_orders_next_step (next_step_at)
) ENGINE=InnoDB;

-- 8. ORDER STATUS HISTORY
-- One row for every status change (checkout writes the first PLACED row), in the same transaction as the change.
-- changed_by: SYSTEM = delivery thread, ADMIN = admin panel, CUSTOMER = the customer (checkout, cancel).
CREATE TABLE order_status_history (
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

-- 9. ORDER ITEMS
-- Stores products that were purchased.
CREATE TABLE order_items (
    order_item_id INT AUTO_INCREMENT PRIMARY KEY,
    order_id      INT NOT NULL,
    product_id    INT NOT NULL,
    quantity      INT NOT NULL,
    price         DECIMAL(10,2) NOT NULL,
    FOREIGN KEY (order_id)   REFERENCES orders(order_id),
    FOREIGN KEY (product_id) REFERENCES products(product_id),
    UNIQUE(order_id, product_id),
    CONSTRAINT chk_order_items_quantity CHECK (quantity > 0),
    CONSTRAINT chk_order_items_price CHECK (price >= 0)
) ENGINE=InnoDB;

-- 10. PAYMENTS
-- Stores payment information for orders.
CREATE TABLE payments (
    payment_id INT AUTO_INCREMENT PRIMARY KEY,
    order_id   INT NOT NULL,
    amount     DECIMAL(10,2) NOT NULL,
    method     VARCHAR(30) DEFAULT 'COD',
    status     VARCHAR(20) NOT NULL DEFAULT 'PENDING',
    -- NULL until the payment is actually made; the server sets it then.
    paid_at    TIMESTAMP NULL DEFAULT NULL,
    FOREIGN KEY (order_id) REFERENCES orders(order_id),
    UNIQUE(order_id),
    CONSTRAINT chk_payments_amount CHECK (amount >= 0),
    CONSTRAINT chk_payments_status CHECK (status IN ('PENDING', 'PAID', 'REFUNDED', 'CANCELLED'))
) ENGINE=InnoDB;

-- 11. REQUEST_LOGS
-- One row per request, written in batches by the server's logger thread (never by the request itself).
-- path is stored WITHOUT the query string. user_id is NULL for visitors who are not logged in.
-- latency_ms = accept -> reply sent, queue_wait_ms = accept -> a worker picked the request up.
-- created_at is the time of the request (the logger sets it), not the time of the insert.
-- No foreign key on purpose: a log table must never block deleting users.
CREATE TABLE request_logs (
    log_id        BIGINT AUTO_INCREMENT PRIMARY KEY,
    method        VARCHAR(10)   NOT NULL,
    path          VARCHAR(255)  NOT NULL,
    status        SMALLINT      NOT NULL,
    latency_ms    DECIMAL(10,3) NOT NULL,
    queue_wait_ms DECIMAL(10,3) NOT NULL,
    worker_id     INT           NOT NULL,
    user_id       INT           NULL,
    created_at    TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_request_logs_created (created_at),
    CONSTRAINT chk_request_logs_status CHECK (status BETWEEN 100 AND 599),
    CONSTRAINT chk_request_logs_times CHECK (latency_ms >= 0 AND queue_wait_ms >= 0)
) ENGINE=InnoDB;

-- 12. ADMIN ACTIONS (audit trail)
-- One row for every change an admin makes (product add/edit/restock/hide, category change, order status, role change,
-- session end). Shown on Admin > Logs as "Admin activity". Rows are only ever added, never changed.
CREATE TABLE admin_actions (
    action_id  BIGINT AUTO_INCREMENT PRIMARY KEY,
    admin_id   INT NOT NULL,
    action     VARCHAR(50)  NOT NULL,
    details    VARCHAR(255) NOT NULL DEFAULT '',
    created_at TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (admin_id) REFERENCES users(user_id),
    INDEX idx_admin_actions_created (created_at)
) ENGINE=InnoDB;

-- 13. STATES
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

-- 14. ADDRESSES (shipping addresses saved by a user)
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

-- 15. ORDER ADDRESSES (the address an order was shipped to)
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
