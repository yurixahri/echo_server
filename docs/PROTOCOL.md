# Echo Server Protocol Specification

> Full request/response field definitions, server handler behavior, and event
> diagrams are maintained in [API_REFERENCE.md](API_REFERENCE.md). This page
> is a server overview.

## 1. Architecture & Design Principles

- **Server-Authoritative:** All rooms, messages, user states, and permissions are enforced exclusively on the backend.
- **Strict Decoupling:** Joining or switching voice rooms never interferes with or resets active chat rooms.
- **Maximum Privacy:** Hard deletion removes the database row and attempts to remove its attachment file.
- **Realtime Permission Enforcement:** Power levels and permissions are rechecked on every routing loop and datagram delivery.

---

## 2. Server Execution & Ports

```bash
# Default (port 5555 for TCP and UDP):
./echo_server

# Custom port:
./echo_server -p 8080
./echo_server --port 8080
```

- **Control Channel (TCP):** Port `N` (ASN.1 DER encoded, 2-byte big-endian length prefix).
- **Voice Channel (UDP):** Port `N` (Low-latency Opus datagrams with 29-byte packed header).

---

## 3. Client State Machine

Each connection (`QTcpSocket*`) is tracked in `client_state_map`:

```
           [TCP Connect]
                 │
                 ▼
          WAITTING_INIT
                 │
      ┌──────────┴──────────┐
      │ Existing UUID       │ New Client (empty UUID)
      ▼                     ▼
   SUCCESS            WAITTING_PROOF
                            │
                            │ Proof-of-Work Solved
                            ▼
                         SUCCESS
```

1. **`WAITTING_INIT`:** Accepts only `client-check-id` `[3]`. Checks optional server password.
2. **`WAITTING_PROOF`:** Server generates a 32-byte challenge and SHA-256 difficulty of 16 leading zero bits. Client must return `new-user-proof` `[5]` before expiration (30 seconds).
3. **`SUCCESS`:** Full access granted; permits room, chat, file, and voice operations according to role permissions.

---

## 4. ASN.1 Protocol Dispatch Table

| Tag | Protocol Message | Server Handler | Description |
|---|---|---|---|
| `[0]` | `chat` | `handle_chat_message` | Inserts message to SQLite, broadcasts to room members. Enforces anti-spam (5 msgs/2s, 5s timeout). |
| `[3]` | `client-check-id` | `check_user_id` | Authenticates existing user or issues PoW challenge. |
| `[5]` | `new-user-proof` | `check_new_user_proof` | Verifies SHA-256 proof; registers new user and allocates ID. |
| `[6]` | `claim-admin-privilege` | `claim_admin_privilege` | Validates one-time setup token and assigns permanent admin rights. |
| `[10]` | `create-room` | `handle_create_room` | Validates `u_can_create_rooms` permission and inserts room. |
| `[11]` | `join-room` | `handle_join_room` | Validates room password and chat/voice join power. `limit-clients` is stored but currently not enforced. |
| `[12]` | `update-server-settings`| `handle_update_server_settings`| Admin updates server name and password. |
| `[13]` | `leave-room` | `handle_leave_room` | Leaves chat (0) or voice (1) room; cleans up routing tables. |
| `[14]` | `file-upload-chunk` | `handle_file_upload_chunk` | Receives chunked file data; writes to `files/room/<room_id>/`. |
| `[15]` | `file-download-req` | `handle_file_download_req` | Sends file chunks (32 KiB max) to authorized room occupants. |
| `[17]` | `history-req` | `handle_history_req` | Retrieves paginated messages `WHERE id < before_id LIMIT ?`. |
| `[18]` | `delete-msg` | `handle_delete_msg` | Hard-deletes message row and unlinks file from disk. |
| `[19]` | `get-my-info` | `handle_get_my_info` | Returns user's own UUID, registration timestamp, and uptime. |
| `[21]` | `avatar-upload-chunk` | `handle_avatar_upload_chunk` | Magic byte check (PNG, JPG, GIF, BMP, WEBP, AVIF); saves to `files/avt/`. |
| `[22]` | `get-roles-req` | `handle_get_roles` | Returns list of all defined server roles. |
| `[24]` | `create-role` | `handle_create_role` | Creates a new role. |
| `[25]` | `update-role-perms` | `handle_update_role_perms` | Updates permissions and triggers `recheck_active_user_permissions()`. |
| `[26]` | `delete-role` | `handle_delete_role` | Deletes role and cascades user role mappings. |
| `[27]` | `get-user-roles-req` | `handle_get_user_roles` | Returns assigned status for target user. |
| `[29]` | `assign-user-roles` | `handle_assign_user_roles` | Updates user roles and enforces permissions in realtime. |
| `[30]` | `kick-user` | `handle_kick_user` | Checks `u_can_kick_users` and removes the target from voice; it does not disconnect the client. |
| `[31]` | `update-room` | `handle_update_room` | Updates room configuration. Password changes kick active occupants. |
| `[32]` | `poke-user` | `handle_poke_user` | Routes poke event to target user socket. |
| `[33]` | `get-members-req` | `handle_get_members` | Returns sorted member list with online and admin indicators. |
| `[34]` | `get-member-detail-req` | `handle_get_member_detail` | Returns detailed member card including assigned role names. |

---

## 5. Voice UDP Server Pipeline

1. **Datagram Arrival:** `voice_udp_server->receiveDatagram()`.
2. **Header Parsing:** Reads 29-byte packed header (`voice_packet_header_t`).
3. **Validation:** Verifies `user_id` matches active client state and `room_id` matches current voice channel.
4. **Permission Check:** Evaluates `u_speak_power >= channel_speak_power`. If insufficient, packet is dropped.
5. **Fan-Out:** Re-broadcasts datagram to all other authenticated clients currently in the same voice channel.
