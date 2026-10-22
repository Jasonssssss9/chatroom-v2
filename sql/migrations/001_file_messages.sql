-- Apply once to a database created before file transfers were implemented.
ALTER TABLE messages
    MODIFY client_message_id VARCHAR(64) COLLATE utf8mb4_0900_bin NULL,
    ADD COLUMN file_id BIGINT UNSIGNED NULL AFTER group_id,
    ADD CONSTRAINT messages_file FOREIGN KEY (file_id) REFERENCES files(id);
