# Deskio simulated-computer tests

Two Python files:
- register_user.py: registers an account, then disconnects.
- simulated_computer.py: logs in and registers its computer; agent, list and controller modes.

The previous C++ server has no device-list or explicit session-selection handlers.
THIS PACKAGE INCLUDES THE REQUIRED SERVER UPDATE. Deploy it before these tests.
The client checks capabilities and exits clearly if you are still running the old server.

## 1. Update your C++ project and database

Replace src/main.cpp, src/database.cpp, src/database.hpp, CMakeLists.txt and
Dockerfile with the versions in this package. Keep your other project files.
Run schema.sql in Neon's SQL Editor for the database specified by DATABASE_URL.
It retains the existing deskio_users table and adds deskio_devices and its index.
A device is owned by the authenticated user; a different account cannot claim
its stored ID. Device names may be updated when reconnecting. The table keeps
last_seen from registration; it is not a continuously updated heartbeat timestamp.

Keep DATABASE_URL set in Render with Neon's complete TLS-enabled URI. Push and
redeploy the Docker service. /health is a process health check, not a DB probe.
As before, the app is C++23; its database adapter is compiled as C++17 to match
Ubuntu's packaged libpqxx ABI. Both database source files are required.

For local Docker, put DATABASE_URL=<your URI> in an untracked .env file, then:

    docker build -t deskio-relay .
    docker run --rm -p 10000:10000 --env-file .env deskio-relay

Do not commit .env, passwords, database URLs or simulated-computer state files.

## 2. Install Python dependency

    python3 -m venv .venv
    . .venv/bin/activate
    python -m pip install websockets==15.0.1

Commands below default to ws://localhost:10000/ws. For Render, append
--url wss://YOUR-SERVICE.onrender.com/ws to EVERY command.

## 3. Register a user (once)

    python register_user.py --username ravi

Prompts for password (and confirmation), full name, date of birth and address.
DOB is required by the existing schema; use YYYY-MM-DD. Passwords must be at
least 12 UTF-8 bytes. They are prompted without display and are never saved.
An account is not itself a computer: registration script exits after success.

## 4. Simulate computer A (terminal 1)

    python simulated_computer.py --username ravi --name "Computer A" --state-file a.json

Logs in, registers a persistent UUID and stays online as a target. Keep running.

## 5. Simulate computer B (terminal 2)

    python simulated_computer.py --username ravi --name "Computer B" --state-file b.json

Use the SAME account but a DIFFERENT state file. Keep running.

## 6. Simulate computer C as controller (terminal 3)

    python simulated_computer.py --username ravi --name "Computer C" --state-file c.json --mode controller

Lists other online computers belonging to ravi. Enter A or B's numbered entry.
On that target terminal, type y to approve within 30 seconds. Both ends receive
session.ready; controller sends text and binary probes and target returns ACKs.
After 30 seconds (override with --seconds), the controller ends the session and
exits. The target remains online. This tests communications, not screen capture
or keyboard/mouse injection.

To list only (the listing client temporarily registers itself, then exits):

    python simulated_computer.py --username ravi --name "List client" --state-file list.json --mode list

For non-interactive test targets you explicitly trust:

    python simulated_computer.py --username ravi --name "Computer A" --state-file a.json --auto-accept

--auto-accept accepts ALL incoming requests, including cross-user requests;
use only for controlled tests. Normal mode asks on the target terminal.

## 7. Optional cross-user test

Register another account with register_user.py. Start its controller with a fresh
state file and pass the target computer ID printed by the first user's agent:

    python simulated_computer.py --username user2 --name "User2 B2" --state-file b2.json --mode controller --target TARGET-UUID

The caller cannot list the other account's devices. Knowing an ID only permits
sending an approval request. Only the authenticated target connection can accept.
Approval is for this session only. One active or pending session per computer.
The target's terminal displays the requester's authenticated username and device name.

## 8. Identity and disconnect behavior

Each simulated computer needs its own state file. Copies of the Python script
in the SAME folder still share the default computer-state.json unless you pass
--state-file. State files contain only a UUID, never passwords. Reusing a state
file while that computer is online gives device_already_online. Reusing an ID
under another account gives device_id_unavailable. The client does not reconnect
automatically; restart with the same state file to reuse its computer identity.

A disconnected computer disappears from the ONLINE list when the disconnect is
detected; its database registration remains. Open WebSockets use keepalives,
but abrupt network failures may take time to detect. Closing either participant
ends its session and notifies the other. Pending requests expire after 30 seconds.
No pending or active session is restored after a server restart.

## Protocol implemented by the included C++ server

First message: registration/login JSON as in the previous milestone.
Success advertises capabilities: ["devices", "sessions"]. Then:

    {"type":"device.register","device_id":"<UUID>","name":"Computer A"}
    {"type":"device.list"}
    {"type":"session.request","target_device_id":"<UUID>"}
    {"type":"session.respond","request_id":"<server-generated ID>","accepted":true}
    {"type":"session.data","session_id":"<ID>","data":{"message":"hello"}}
    {"type":"session.end","session_id":"<ID>"}

Only a registered live device can request sessions. Lists contain this user's
ONLINE devices, including itself (marked is_self). Both accounts' requests
require target acceptance; there is no implicit same-account auto-pairing now.
Text relay is wrapped in session.data and arbitrary binary messages are only
forwarded inside an approved active session. This is a protocol change from the
previous bare two-slot relay. Old clients must be updated. There is no remote
control implementation in these Python files; successful probes only establish
bidirectional test-message delivery.

## Limitations

This is a development milestone, not production authorization infrastructure.
Account login is currently sufficient to register devices; no additional device
key, remembered trust, JWT refresh, recovery, multi-instance routing or durable
session resumption is implemented. Keep public connections on WSS. Existing
basic limits allow 60 authentication/device-registration jobs per server minute
and 8 outstanding jobs; more complete abuse protection remains future work.
Only ONLINE own-device lists are implemented here, not a listing of offline devices.

Agent approval uses a terminal input prompt. If a request expires while you are
reading it, answering later is rejected; press Enter to resume receiving events.
Do not reuse one simulated device's state file across separate accounts.

## Validation performed

The C++ server compiled and linked against Ubuntu dependencies (Boost 1.83,
libpqxx 7.8 and libsodium). Both Python files pass syntax compilation. Network
integration checks used the actual compiled server with a test-only database
adapter: five live computers across two users, owner-filtered lists, duplicate
and other-owner device rejection, blocking data before approval, rejecting a
spoofed approval, declining requests, approved same-account/cross-account text
and binary relay, busy targets, explicit session end, and disconnect cleanup.
Both delivered Python scripts also ran against this fixture, including three
simultaneous computers and acknowledgements in both formats. The fixture adapter
is not in this package; deployed code uses PostgreSQL. Real Neon persistence and
the Docker build were not exercised in the authoring environment.
