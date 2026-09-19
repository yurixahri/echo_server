-- 1. USER IDENTITY & GLOBAL POWER
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    uuid TEXT UNIQUE NOT NULL,   
    name TEXT NOT NULL,
    avatar_path TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
	last_seen DATETIME NOT NULL,
    is_admin BOOLEAN DEFAULT 0
);

CREATE TABLE rooms (
    id INTEGER PRIMARY KEY AUTOINCREMENT,     
    name TEXT NOT NULL,
    password TEXT,                 
    limit_clients INTEGER DEFAULT 0
);

-- 3. PERMISSIONS (TeamSpeak Style)
-- Use this for granular control: "i_channel_join_power", "i_client_ban_power", etc.
CREATE TABLE permissions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL
);

CREATE TABLE roles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT UNIQUE NOT NULL
);

CREATE TABLE role_permissions (
    role_id INTEGER,
    permission_id INTEGER,
    value INTEGER NOT NULL,
    FOREIGN KEY(role_id) REFERENCES roles(id) ON DELETE CASCADE,
    FOREIGN KEY(permission_id) REFERENCES permissions(id) ON DELETE CASCADE,
    PRIMARY KEY(role_id, permission_id)
);


CREATE TABLE user_roles (
    user_id INTEGER,
    role_id INTEGER,
    FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY(role_id) REFERENCES roles(id) ON DELETE CASCADE,
    PRIMARY KEY(user_id, role_id)
);

-- 4. MESSAGE STORE (The "Heavy" Table)
CREATE TABLE messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    room_id INTEGER NOT NULL,
    sender_id INTEGER NOT NULL,
    
    -- BLOB contains [Zstd(Encrypted(Text))]
    -- If deleted, we replace this with a tiny 1-byte 'DELETED' marker to save space
    payload BLOB NOT NULL, 
    
    type INTEGER DEFAULT 0,             -- 0: Text, 1: Image Link, 2: File
    is_edited BOOLEAN DEFAULT 0,
    is_deleted BOOLEAN DEFAULT 0,
    
    server_timestamp INTEGER NOT NULL,  -- Unix Epoch for faster sorting/filtering
    
    FOREIGN KEY(room_id) REFERENCES rooms(id) ON DELETE CASCADE,
    FOREIGN KEY(sender_id) REFERENCES users(id)
);

CREATE TABLE server_settings (
    key TEXT PRIMARY KEY,
    value TEXT
);


-- INDEXES FOR SPEED
CREATE INDEX idx_msg_room_time ON messages(room_id, server_timestamp);
CREATE INDEX idx_user_uuid ON users(uuid);