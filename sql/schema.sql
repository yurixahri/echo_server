-- 1. USER IDENTITY & GLOBAL POWER
CREATE TABLE users (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    uuid            TEXT UNIQUE NOT NULL,
    name            TEXT NOT NULL,
    avatar_path     TEXT,
    created_at      INTEGER DEFAULT (unixepoch()),
	last_seen       INTEGER DEFAULT (unixepoch()),
    is_admin        BOOLEAN DEFAULT 0
    status          INTEGER DEFAULT 0
);

CREATE TABLE rooms (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,     
    name            TEXT NOT NULL,
    password        TEXT,                 
    limit_clients   INTEGER DEFAULT 0
);

CREATE TABLE permissions (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT UNIQUE NOT NULL
);

CREATE TABLE roles (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT UNIQUE NOT NULL
);

CREATE TABLE role_permissions (
    role_id         INTEGER,
    permission_id   INTEGER,
    value           INTEGER NOT NULL,
    FOREIGN         KEY(role_id) REFERENCES roles(id) ON DELETE CASCADE,
    FOREIGN         KEY(permission_id) REFERENCES permissions(id) ON DELETE CASCADE,
    PRIMARY         KEY(role_id, permission_id)
);


CREATE TABLE user_roles (
    user_id         INTEGER,
    role_id         INTEGER,
    FOREIGN         KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN         KEY(role_id) REFERENCES roles(id) ON DELETE CASCADE,
    PRIMARY         KEY(user_id, role_id)
);

-- 4. MESSAGE STORE (The "Heavy" Table)
CREATE TABLE message_type (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT UNIQUE NOT NULL
);


CREATE TABLE messages (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    room_id             INTEGER NOT NULL,
    sender_id           INTEGER NOT NULL,
    
    payload             BLOB NOT NULL, 
    
    type                INTEGER NOT NULL,             -- 0: Text, 1: Image Link, 2: Video, 3: Others
    is_edited           BOOLEAN DEFAULT 0,
    is_deleted          BOOLEAN DEFAULT 0,
    
    server_timestamp    INTEGER DEFAULT (unixepoch()),  -- Unix Epoch for faster sorting/filtering
    
    FOREIGN             KEY(room_id) REFERENCES rooms(id) ON DELETE CASCADE,
    FOREIGN             KEY(sender_id) REFERENCES users(id),
    FOREIGN             KEY(type) REFERENCES message_type(id)
);

CREATE TABLE server_settings (
    key             TEXT PRIMARY KEY,   -- For claiming admin privileges
    value           TEXT
);


INSERT INTO message_type(id, name) VALUES
    (1, 'text'),
    (2, 'image'),
    (3, 'video'),
    (4, 'other')
ON CONFLICT DO UPDATE SET name = excluded.name;

INSERT INTO permissions (id, name) VALUES
	(1, 'u_can_create_rooms'),
	(2, 'u_can_delete_rooms'),
	(3, 'u_can_ban_users'),
	(4, 'u_speak_power'),
	(5, 'u_join_power'),
	(6, 'channel_join_power'),
	(7, 'channel_speak_power')
ON CONFLICT DO UPDATE SET name = excluded.name;


-- INDEXES FOR SPEED
CREATE INDEX idx_msg_room_time ON messages(room_id, server_timestamp);
CREATE INDEX idx_user_uuid ON users(uuid);