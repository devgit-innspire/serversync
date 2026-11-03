These files are only for old databases, new setups use ecommerce_db.sql (in the folder above).

If you have an old database made before a change and want to keep its data, run the files you are missing, in number order,
as described at the top of each file (`mysql -u <user> -p --default-character-set=utf8mb4 ecommerce_db < sql\migrations\alter_00X_....sql`).
`alter_002` and `alter_006` are replaced by `alter_008` (it sets the final list of order statuses), so skip them if you run 008.
`alter_010` added `users.must_change_pw` and `alter_011` drops it again (that feature was removed): run both only if your database
already has the column, otherwise skip both.
`alter_012` adds UPI / Card / Net Banking payments (`payments.pay_detail`, `payments.txn_ref` and a CHECK on `method`).
