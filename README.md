# Deskio registration and login milestone

Replacement source for the C++23 Boost.Beast Docker relay discussed in chat.
Uses PostgreSQL/libpqxx, nlohmann/json and libsodium Argon2id password hashing.
No database secret is embedded in the source or image.

## Files to replace/add in your existing project

- Replace `src/main.cpp`, `CMakeLists.txt`, `Dockerfile`, `.dockerignore`.
- Add `src/database.hpp` and `src/database.cpp` (both required by the build).
- Add `schema.sql` and optional `test_auth.py`.
- Keep your own unrelated project files. This is not a patch against an uploaded repo.

## Database setup (once, before starting the server)

In Neon's SQL Editor, run the contents of `schema.sql` against the same database
and branch named in DATABASE_URL. `deskio_users` stores registration records.
CREATE TABLE IF NOT EXISTS does not migrate an incompatible pre-existing table.

In Render's Environment settings, set DATABASE_URL to the complete Neon
connection URI, including all TLS parameters. Never commit it to Git. The server
adds connect_timeout=10 if absent. Keep Neon TLS enabled. For a local PostgreSQL
container, use a local connection URI instead.

## Render deployment

Push the replacement files and deploy as Docker. Keep health path `/health`.
The endpoint `/health` checks process liveness, not database connectivity.
Missing DATABASE_URL causes startup to fail. Database unavailability/missing
schema returns auth.error/service_unavailable without exposing credentials.

## Local Docker

Create an untracked `.env` file containing DATABASE_URL=<your full URI>.
Add `.env` to your Git ignore rules. Do not put quotes around the URI in this
Docker env file. Preserve its query parameters. Then:

    docker build -t deskio-relay .
    docker run --rm -p 10000:10000 --env-file .env deskio-relay

Use ws://localhost:10000/ws locally; use wss://<your-host>/ws publicly.
Render handles public TLS; this executable itself does not terminate TLS.

## First WebSocket message

The HTTP WebSocket upgrade completes first. The connection cannot relay data
until its first authentication exchange succeeds. Authentication is attached to
that connection only; reconnect and authenticate again. No JWT is issued yet.

Registration (explicit type is recommended):

    {"type":"register","username":"ravi","password":"example-password-123",
     "name":"Ravindra Singh","dob":"1990-01-15","address":"Example address"}

Login:

    {"username":"ravi","password":"example-password-123"}

`type: login` is also accepted. If type is omitted, the presence of any of name,
dob or address selects registration; all three must then be supplied. Username
and password alone always selects login: unknown accounts are NOT auto-created.
Username is canonicalized to lowercase and restricted to 3-64 ASCII letters,
digits, _, . and -. Registration passwords are 12-1024 UTF-8 bytes; login accepts
1-1024 bytes. No password trimming is performed. Name and address are strings;
DOB is a real calendar date, YYYY-MM-DD, from 1900 through today (UTC).

Success:

    {"type":"auth.success","registered":true,
     "user":{"id":1,"username":"ravi"},"relay_available":true}

On subsequent logins, registered is false. Profile details and hashes are not
returned. An error looks like:

    {"type":"auth.error","code":"invalid_credentials"}

Other codes: username_unavailable, invalid_request, rate_limited, server_busy,
service_unavailable. Read the response before sending relay traffic.

## Relay behavior in this milestone

Up to two authenticated connections PER USER are automatically paired, preserving
the previous two-computer demo while preventing automatic cross-account forwarding.
Text/binary payloads after login are forwarded unchanged to that user's other
paired connection. Messages without a peer are dropped. Additional connections
can authenticate but receive relay_available=false and do not relay; reconnect
after a slot becomes available. There is no automatic slot reassignment.

This does NOT yet implement the device registry, persistent device credentials,
device lists, explicit session selection, or approved cross-user control from the
architecture discussion. Cross-user traffic stays blocked until that approval
workflow is implemented. Do not use the automatic demo pairing as a remote-control
consent policy.

## Interactive test

    python3 -m venv .venv
    . .venv/bin/activate
    python -m pip install websockets==15.0.1
    python test_auth.py --register --label computer1

Then in a second terminal, log in to the SAME account:

    python test_auth.py --label computer2

For Render, add --url wss://<your-host>/ws to both commands. Supply test profile
data when experimenting. The client prompts for passwords without displaying
them. Both clients run for 30 seconds after authentication.
The old computer1.py/computer2.py probes do not authenticate and cannot be used
unchanged with this version.

## Implementation limits

A dedicated one-thread worker performs password hashing and synchronous SQL,
with at most 8 queued/running authentication jobs. The IO event loop remains
single-threaded. Each auth operation uses its own DB connection, not one per
WebSocket. SQL parameters are bound rather than interpolated. Password hashing
uses libsodium's interactive settings (64 MiB per operation).

The WebSocket application is C++23. The small database adapter is compiled as
C++17 to match Ubuntu 24.04's packaged libpqxx 7.8 exception ABI; otherwise C++20+
source-location declarations in its headers can cause unresolved symbols. CMake
sets this per target. Do not compile database.cpp with the main app's C++23 flags
when using this packaged library.

An unauthenticated socket expires after 60 seconds, allows five attempts with
2-second spacing, and is limited to 8 KiB messages. There is also an instance-wide
60-auth-attempts/minute cap. This is a prototype baseline, not a complete abuse
control system: use account/source limits before public rollout. No account
recovery, email verification, long-term token/revocation system or audit log yet.
The database stores profile fields as ordinary columns; this code does not add
application-level encryption for address/DOB. Logs never intentionally print
request bodies, passwords, database URIs, or SQL exception details.

Run schema migrations separately. Removing a container does not remove the
managed database or its users. Keep backups and retention policy appropriate to
the data you collect.

## Validation performed

Compiled and linked with GCC 13, Boost 1.83, libpqxx 7.8 and libsodium on Ubuntu
dependencies. The Python client passes syntax compilation. Live checks against
the compiled C++ server passed: HTTP health, malformed JSON (without reflecting
the supplied secret), binary-before-login rejection, missing credentials,
invalid calendar dates, short registration passwords, SQL metacharacters in
usernames, safe database-unavailable errors for registration/login, and blocking
unauthenticated relay forwarding. Docker itself is unavailable in the authoring
environment. Successful database inserts, duplicate-account handling and login
against a real PostgreSQL/Neon database remain to be verified using your own
DATABASE_URL; no access to your database was requested or used.
