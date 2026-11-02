# Echo Server Architecture & File Index

## 1. Overview

`echo_server` is a headless, lightweight C++20 and Qt 6.11 server backend managing stateful client TCP sessions, SQLite database storage in WAL mode, and real-time UDP voice routing.

For the complete TCP/UDP API, wire fields, server handler behavior, and event
diagrams, see [API_REFERENCE.md](API_REFERENCE.md).

---

## 2. Core Entry

- **[main.cpp](../main.cpp)**
  - Server entry point.
  - Parses command-line port flags (`-p` / `--port <port>`, default: `5555`).
  - Loads ASN.1 definitions (`ChatProtocol.asn`).
  - Initializes SQLite database and automated migrations (`initialize_database()`).
  - Registers ASN.1 packet event handlers (`def.on(...)`).
  - Binds dedicated UDP voice worker thread (`init_voice_server(port)`).
  - Starts `QTcpServer` listening on the specified port.

---

## 3. Database Subsystem (`db/` & `sql/`)

- **[db/db.h](../db/db.h)**
  - Configures SQLite in `WAL` journal mode with `NORMAL` synchronicity.
  - Executes initial schema creation from `sql/schema.sql` on fresh setup.
  - Generates secure random setup token for claiming initial server admin privileges.
  - Automatically executes incremental schema migrations (e.g. adding `needed_voice_join_power`, `needed_chat_join_power`, `needed_speak_power` to `rooms`, and populating default roles `Guest`, `Member`, `Admin`).
  - Provides scalar and update query execution helpers (`db_scalar`, `db_update`).

- **[db/user.h](../db/user.h)**
  - User verification and challenge proof solver validator.

- **[sql/schema.sql](../sql/schema.sql)**
  - Relational schema defining:
    - `users`: ID, UUID, name, avatar path, admin flag, timestamps.
    - `rooms`: ID, name, password, limit, join/speak power requirements.
    - `permissions`: Permission ID and name mapping.
    - `roles`: Custom role entities.
    - `role_permissions`: Role-to-permission mapping with power values.
    - `user_roles`: User assignment to roles (cascades on delete).
    - `messages`: Message payloads, types, room associations.
    - `server_settings`: Key-value storage for server configuration.

---

## 4. Functional Subsystems (`functions/`)

- **[functions/room.h](../functions/room.h)**
  - Handles room operations: create, join, leave, update, and deletion.
  - Formats and sends room list updates with natural sorting (`natural_sort_less_than`).
  - Enforces room password protection, client capacity limits, and join power.
  - When room password is updated, active occupants are kicked and prompted to re-authenticate.
  - Manages chat message persistence, retrieval paging, and anti-spam rate limiting (5 msgs/2s, 5s timeout).
  - Handles message hard-deletion (removes database record and immediately unlinks associated disk files).

- **[functions/user.h](../functions/user.h)**
  - Manages client connection lifecycle, UUID validation, and PoW challenge generation/verification.
  - Handles admin privilege claiming via one-time token.
  - Generates member list rosters and user detail profile cards (including assigned role names).
  - Routes poke notifications between clients.

- **[functions/voice.h](../functions/voice.h)**
  - Dedicated UDP voice server thread running an event loop on `QUdpSocket`.
  - Dispatches voice datagrams based on room membership.
  - Enforces `u_speak_power >= channel_speak_power` on every datagram to ensure real-time mute enforcement without room re-entry.

- **[functions/file.h](../functions/file.h)**
  - Manages chunked file uploads and downloads.
  - Verifies magic byte signatures for avatar uploads (PNG, JPEG, GIF, BMP, WEBP, AVIF).
  - Stores room attachments in `files/room/<room_id>/` and user avatars in `files/avt/`.
  - When user replaces their avatar, automatically purges the old avatar file from disk.

- **[functions/role.h](../functions/role.h)**
  - CRUD operations for roles and permissions.
  - Computes effective user permissions: `effective_perm = max(user_role_values)`.
  - `recheck_active_user_permissions()`: Scans all connected clients and kicks users from voice/chat rooms if their updated permissions drop below room requirements.

---

## 5. Common & Utilities (`common/` & `utils/`)

- **[common/types.h](../common/types.h)**
  - Shared enums and structures: `client_state_t`, `voice_user_t`, `role_t`, `user_role_item_t`.
  - Natural sorting helper using `QCollator`.

- **[common/functions.h](../common/functions.h)**
  - Helper to construct and send standardized `Response` ASN.1 packets (`send_tcp_response_event`).

- **[utils/asn1_parser.h](../utils/asn1_parser.h)**
  - ASN.1 DER parser and serializer wrappers for the server socket listener.
