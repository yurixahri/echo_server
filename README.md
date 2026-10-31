# Echo Server

Headless, stateful, and server-authoritative backend daemon for the Echo voice and chat platform. Built with C++20, Qt 6 (Network, Core, Sql), and SQLite in WAL mode.


## Quick Review

- **Server-Authoritative State:** Centralized permission verification, room sessions, and user records.
- **Dual Transport Architecture:**
  - **TCP Control Channel:** ASN.1 DER-encoded control protocol with length-prefixed framing.
  - **UDP Voice Channel:** Low-latency Opus audio routing with 29-byte packed headers and realtime power enforcement.
- **Strict Decoupling:** Voice routing and chat rooms exist on separate layers; changing voice channels never interrupts chat sessions.
- **Privacy-First Storage:** Hard-deletions immediately unlink physical files from disk and delete database rows.
- **Proof-of-Work Registration:** Client UUID allocations are gated by dynamic SHA-256 challenges to mitigate spam.
- **Automated Database Migrations:** SQLite schema initializes and migrates automatically without data loss.



## Building from Source

### Prerequisites

- **Compiler:** GCC 13+ (MinGW-w64 on Windows or native GCC/Clang on Linux) with C++20 support.
- **Qt Version:** Qt 6.5+ (Core, Network, Sqlite).
- **Libraries:**
  - `libtasn1`
  - `zlib` (Not use, for now)

### Build Instructions

##### Windows
To build this project, please install msys2 and get the lasted [mingw-w64-x86_64-qt6-base](https://packages.msys2.org/packages/mingw-w64-x86_64-qt6-base) and [mingw-w64-x86_64-qt6-static](https://packages.msys2.org/packages/mingw-w64-x86_64-qt6-static), this is a static build project. You can use Qtcreator from mingw for gui, or just use qmake and build:

```bash
# 1. Clone repository
git clone https://github.com/yurixahri/echo.git
cd echo

# 2. Run qmake with your static Qt installation
qmake echo.pro -spec win32-g++ "CONFIG+=release"

# 3. Compile
mingw32-make -j$(nproc)
```

The resulting binary will be output to `release/echo_server` (or `release/echo_server.exe`).
Only support Windows for now.



## Usage

### Running the Server

```bash
# Default (runs on port 5555 for TCP and UDP):
./echo_server

# Custom port:
./echo_server -p 8080
./echo_server --port 8080
```

### Initial Administrator Setup

Upon the very first launch, the server generates an administrative claim token:

```
====================================================================
 SERVER STORAGE SYSTEM INITIALIZED SUCCESSFULLY!
 No administrative account exists yet.
 Use this temporary privilege key inside your client app to claim admin power:
 ---> XXXXX-XXXXX-XXXXX-XXXXX <---
====================================================================
```

1. Launch the Echo client and connect to your server.
2. Open the server menu (`⋯`) in the top header and select **Claim Admin**.
3. Paste the privilege key to grant permanent server administrator permissions to your account.


## Reverse proxy
### Windows

Nginx on windows does not support udp, so instead use [frp]([https://gost.run/en/](https://github.com/fatedier/frp)) (kind of a work around than official way to do, but it work).
Assume your host port is 5556 and your app local port is 5555, your frps.toml and frpc.toml would be like:
```toml
# frps.toml
bindPort = 7000

[log]
# Remove # to turn off log
# to = "NUL"          # Windows (use "/dev/null" on Linux)
# level = "error"     # Suppress info/debug
```

```toml
# frpc.toml
serverPort = 7000

[log]
# Remove # to turn off log
# to = "NUL"          # Windows (use "/dev/null" on Linux)
# level = "error"     # Suppress info/debug

[[proxies]]
name = "echo_tcp_tls"
type = "tcp"
remotePort = 5556

[proxies.plugin]
type = "tls2raw"
localAddr = "127.0.0.1:5555"
crtPath = "/path/to/yourdomain/fullchain.pem"
keyPath = "/path/to/yourdomain/privkey.pem"

[[proxies]]
name = "echo_udp_voice"
type = "udp"
localIP = "127.0.0.1"
localPort = 5555
remotePort = 5556

```
Then run
```
frps.exe -c frps.toml
frpc.exe -c frpc.toml
```

### Linux
Since I have not compiled in linux yet, I can't test it out, but let assume it works, use NginX
```
stream {
    upstream echo_backend_tcp {
        server 127.0.0.1:5555;
    }

    upstream echo_backend_udp {
        server 127.0.0.1:5555;
    }

    server {
        listen 5556 ssl;
        proxy_pass echo_backend_tcp;

        ssl_certificate     /path/to/yourdomain/fullchain.pem;
        ssl_certificate_key /path/to/yourdomain/privkey.pem;
        ssl_protocols       TLSv1.2 TLSv1.3;
        ssl_ciphers         HIGH:!aNULL:!MD5;
        ssl_session_cache   shared:SSL_STREAM:10m;
        ssl_session_timeout 4h;

        proxy_timeout         86400s;
        proxy_connect_timeout 5s;
    }

    server {
        listen 5556 udp;
        proxy_pass echo_backend_udp;
        proxy_timeout   86400s;
        proxy_responses 0;
    }
}
```
## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
