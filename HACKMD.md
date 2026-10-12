System Programming 2026: csieLedger

    Revision: 2026-09-30 — four TODOs, update sessions with close, immediate transfer lock failure; total 7 points.

tags: NTU-SP

    System Programming 2026: csieLedger
            Understanding the Starter
        0. Quick Summary
        1. Goal and Architecture
        2. Release Files and Build
        3. Account File
        4. Protocol and Per-Client State
        5. TODO 1 / Public Task 1 — Commands and Update Sessions (1 point)
            5.1 READY commands
            5.2 add: apply immediately and keep the session
            5.3 close: release without rollback
            5.4 Releasing a session versus disconnecting
        6. TODO 2 / Public Task 2 — Nonblocking Record Locks (3 points)
        7. TODO 3 / Public Task 3 — Multiplexing and Input (1 point)
        8. TODO 4 / Public Task 4 — Confirmed Transfer with Immediate Lock Failure (2 points)
            8.1 Registration
            8.2 Request and acquisition
            8.3 Lock conflict: fail this request without pairing
            8.4 Offer and confirmation
            8.5 Paired cancellation and disconnection
            8.6 State table
            8.7 Scope and grading cases
        9. Invalid Input and Cleanup
            9.1 Failed response writes
        10. Public Checker and Grading
        11. Submission
            11.1 Late Policy
        12. FAQ
        13. Questions and Help

Important Information

    Deadline: October 15, 2026, 23:59 (Taiwan time, UTC+8)
    Late submission cutoff: October 22, 2026, 23:59 (Taiwan time, UTC+8)
    Discussion / Q&A: GitHub Issues
    Submission: {NTU_COOL_SUBMISSION_LINK}
    Grading environment: {ws1 / designated workstation}
    Starter repository: SP2026_HW1_release

Spaces, capitalization, newlines, and prompts are part of the protocol.
Understanding the Starter

You are not expected to understand the entire starter script before beginning Task 1.

A useful approach is:

    Read the client states in Section 4, and draw a state diagram for better understanding.
    Find the request structure in server.h (one request represents one connected client).
    Identify the command handlers and how they move a client between states.
    Identify the cleanup helpers used when a session ends or a client disconnects.
    Find record_owner[] and lock-related helpers (Task 2).
    Find append_input() / pop_command() (Task 3).
    Examine the transfer-related logic (Task 4).

Most TODOs are implementing state transitions, ownership tracking, or cleanup behavior.
0. Quick Summary

In this assignment, you will extend csieLedger, a TCP-based account service.
Clients can view account balances, change a balance within an update session,
and transfer money after the receiving client confirms the request.

The main goals are to protect shared account data from conflicting operations
and keep the server responsive while handling multiple clients. You will learn
how file locking and I/O multiplexing support these two goals.
Task 	What you will implement 	Points
1 	Basic account commands and update sessions 	1
2 	Protection of shared account records against conflicting access 	3
3 	Concurrent client handling and correct command reception 	1
4 	Transfers confirmed by the receiving client 	2
	Total 	7

We recommend completing Tasks 1 → 2 → 3 → 4. Each task adds functionality
to the same account server. The following sections define the required command
behavior, implementation requirements, and testing instructions.
1. Goal and Architecture

One server process manages multiple TCP client connections. Multiple server
processes may open the same accountRecord file and coordinate access with
fcntl() byte-range locks. These locks belong to a process, so they cannot
distinguish clients connected to the same server process. Maintain
record_owner[] to track which client owns a record during an update session
or an active transfer, and prevent conflicting access by other clients
connected to that server.

Every accepted connection has its own conn_fd, state, input buffer, and
operation fields. One connection may issue many commands over its lifetime.
An fd may be reused after disconnection; reset all connection fields on reuse.

Different clients may update different accounts in overlapping sessions.
Only one updater or acquired transfer may own a particular account at a time.

Use a single process with a single event loop per server. Do not use fork,
threads, busy waiting, or F_SETLKW. The grader starts separate server
processes on different ports for interprocess lock tests.
2. Release Files and Build

Implement all four tasks in the same server.c. The standard make target
builds an executable named server, and every public task tests that executable.
Rebuild with make after changing the source code; separate executables or
task-specific compilation modes are not required.

Obtain the starter project from GitHub:

git clone https://github.com/ntusp2026/SP2026_HW1_release.git
cd SP2026_HW1_release

The provided sequential loop supports developing Tasks 1 and 2. In Task 3,
replace it with the required select() event loop so the server can handle
multiple clients concurrently.

The main starter files are:

SP2026_HW1_release/
├── Makefile
├── README.md
├── accountRecord
├── checker.py
├── server.c
└── server.h

The latest assignment specification is maintained on
HackMD.
The repository's README.md links to this specification.

Search server.c for TODO 1–4.
The starter compiles but intentionally does not pass the tests.

Supplied infrastructure includes socket creation/bind/listen, opening the
record file, per-client storage, strict parsers, record I/O helpers,
append_input()/pop_command(), command dispatch, deferred failed-notification
cleanup. You implement command handlers,
lock/state cleanup, buffer draining, and the select loop.

make                     # produces ./server
./server 7777
# Another terminal:
nc 127.0.0.1 7777
# Another server sharing the same accountRecord, for manual lock tests:
./server 7778
make clean

Use Linux, GCC, make, and Python 3.9+ (standard library only). Compile without
warnings. The default make target must generate an executable named server.
Do not modify the public checker to make your solution pass. Do not hard-code
the released balances. A checker temporarily rewrites and restores the record
file, so do not run it alongside another checker or a manual server in the
same directory.
3. Account File

There are 20 fixed-size records, IDs 902001 through 902020:

typedef struct {
    int id;
    int balance;
} account_record;

On the grading platform this is an 8-byte record with two 32-bit integers.
Balances are between 0 and 1,000,000, inclusive.

index  = account_id - ACCOUNT_ID_START;
offset = index * sizeof(account_record);

Use the supplied read_record_at()/write_record_at() helpers (pread/pwrite).
Do not overwrite the ID or unrelated records. Use a wide intermediate type
when adding a delta or validating a transfer; signed overflow is not allowed.

The initial public data are:

500 1200 0 999999 350 42 7600 18 910 1000000
240 87 650 3000 1 777 25000 64 880 150

These correspond to ascending IDs. Other tests may use different balances.
4. Protocol and Per-Client State

Every command ends with \n; \r\n is also accepted. Commands are
case-sensitive, have no leading/trailing whitespace, and use exactly one
ASCII space between tokens. Integers are decimal values representable by
32-bit signed int; a leading sign is allowed. Account IDs must be in range.
Transfer amounts must be strictly positive; add deltas may be positive, zero,
or negative. Inputs are ordinary text without embedded NUL bytes.

TCP is a byte stream: a command may span multiple reads, and one read may
contain multiple complete commands. A client's outstanding input burst is
at most 512 bytes including line endings and a partial tail. Test clients read
responses normally; full nonblocking output queues are outside scope.

On connection, send exactly:

================================
 Welcome to CSIE Ledger System 
================================
Please enter your command: 

The welcome line has a space before its newline. Prompts have a trailing space
and no terminating newline. Their exact C strings are:

"Please enter your command: "                 /* READY */
"Please enter add <delta> or close: "          /* WAIT_UPDATE */

Client state 	Commands 	State after operation
READY 	read, update, receive, transfer, exit 	Depends on command
WAIT_UPDATE 	add, close, exit 	add stays; close → READY; exit disconnects
WAIT_RECEIVE 	cancel, exit 	cancel → READY; exit disconnects
WAIT_TRANSFER_OUT 	cancel, exit 	Sender awaiting receiver decision
WAIT_TRANSFER_IN 	accept, reject, exit 	Receiver deciding an acquired offer

State is per client. Other commands in that state are invalid and disconnect
that client after cleanup. There is no WAIT_UPDATE_LOCK state.
5. TODO 1 / Public Task 1 — Commands and Update Sessions (1 point)

Complete send_text() first because the welcome message uses it. Write the
entire string, handling short writes and EINTR. Return 0 on success and -1
on failure, including a write that makes no progress. send_text() reports
failure; its caller is responsible for connection cleanup as specified in
Section 9. Do not hide a failed send by continuing the command normally.
5.1 READY commands

read <id> reads a record and stays READY. For read 902001 with balance 500:

">>> Account 902001 balance: 500\nPlease enter your command: "

update <id> begins a session for that account. After acquiring the lock under
Task 2, read its current balance and enter WAIT_UPDATE. With initial balance 500:

">>> Account 902001 balance: 500\n>>> Update lock acquired.\nPlease enter add <delta> or close: "

Only the short update <id> form is valid. update <id> blocking and
update <id> nonblocking contain extra arguments and are invalid.

exit replies >>> Client exit.\n, cleans up, and closes the client socket.
No READY prompt follows. The listening socket and other clients remain active.
5.2 add: apply immediately and keep the session

In WAIT_UPDATE, add <delta> applies to the current balance of this session.
Write each successful result immediately, update the cached current balance,
and stay WAIT_UPDATE. Keep the record lock and local ownership.

For an add that changes 500 to 600:

">>> Update successful.\n>>> Account 902001 balance: 600\nPlease enter add <delta> or close: "

A later add -30 must start from 600 and produce 570. Do not reuse the balance
from the start of the session. Do not defer writing until close.

If the resulting balance would be outside 0–1,000,000, do not change it. Reply:

">>> [Error] Balance out of range.\nPlease enter add <delta> or close: "

Keep the session and lock so the client can try another add or close.
A syntactically invalid delta instead follows the invalid-command rule.
5.3 close: release without rollback

In WAIT_UPDATE, close releases the update record lock, clears its matching
local owner and session fields, and returns to READY on the same connection:

">>> Update closed.\nPlease enter your command: "

It is valid even if no add occurred. It does not undo any successful add.
cancel is invalid in WAIT_UPDATE; close is invalid in READY and transfer states.

Example (arrows label sent commands, not protocol text):

update 902001  → balance 500; WAIT_UPDATE
add 100        → file balance 600; still WAIT_UPDATE, still locked
add -30        → file balance 570; still WAIT_UPDATE, still locked
close          → balance stays 570; unlocked; READY
read 902001    → balance 570
exit           → disconnect

5.4 Releasing a session versus disconnecting

Releasing a record lock is not the same as closing the client connection.
close releases the update session and returns to READY on the same socket.
To disconnect normally after close, send exit separately. You may also send
exit directly during WAIT_UPDATE; a preceding close is not required.

The exit command is valid in both READY and WAIT_UPDATE. If used during
WAIT_UPDATE, the server preserves all successful adds, releases the record
lock and matching local ownership, clears the update-session fields, sends
>>> Client exit.\n, and closes the client connection. No READY prompt follows.

A syntactically invalid command, or a command used in the wrong state, follows
the global invalid-command rule: clean up any active update session, send the
invalid-command response, and close the connection. Detected EOF or an input
error also cleans up and closes the connection; EOF requires no reply.
None of these paths rolls back a successful add.

A syntactically valid add whose resulting balance is out of range is not
an invalid command: it keeps the session and lock active, as in Section 5.2.
Action in WAIT_UPDATE 	Record lock 	Connection 	Earlier successful adds
close 	Released 	Open; READY 	Preserved
exit 	Released 	Closed after exit response 	Preserved
Invalid command 	Released 	Closed after invalid-command response 	Preserved
Detected EOF / input error 	Released 	Closed 	Preserved
Valid add, result out of range 	Retained 	Open; WAIT_UPDATE 	Preserved

For example, after add 25 changes 500 to 525, both close and exit leave
525 in the file. Only exit disconnects the client.

Task 1 combines the former basic-command and update-workflow tasks. Its public
cases include repeated adds, immediate persistence, range errors, close with
and without adds, continued use of the same connection, and exit/invalid cleanup.
6. TODO 2 / Public Task 2 — Nonblocking Record Locks (3 points)

Use traditional POSIX record locks with
fcntl(record_fd, F_SETLK, &lock). Acquire locks without blocking: if a
conflicting lock is already held by another process, F_SETLK returns an error
immediately instead of waiting for that lock to be released. Handle the conflict
as specified below. Do not use F_SETLKW, wait in a loop, sleep until a lock
becomes available, or queue an update for later retry.

Lock exactly the requested record:

l_whence = SEEK_SET;
l_start  = index * sizeof(account_record);
l_len    = sizeof(account_record);

Operation 	Lock type 	Lifetime
read 	F_RDLCK 	Acquire before reading; release after reading
update 	F_WRLCK 	Before initial read, through all adds, until close/cleanup
transfer 	Two F_WRLCK locks 	See Task 4

For read/update, either a local-owner conflict or F_SETLK failure with
EACCES/EAGAIN returns immediately:

">>> Locked.\nPlease enter your command: "

The requester remains READY and acquires no ownership. A failed attempt must
never release somebody else's lock.

An unexpected fcntl() error is an internal operation failure, not a
Locked response. If lock acquisition fails for a reason other than
EACCES/EAGAIN, clean up only resources acquired by the current operation
and close the affected client connection. Do not send Locked, a success
response, or a READY prompt. No additional client-facing error message is
required; diagnostic output to stderr is allowed. This acquisition failure
alone must not terminate the server or disconnect unrelated clients.

If a transfer's second acquisition fails unexpectedly, release its first lock
before closing the sender. Since no offer was established, the receiver keeps
its registration and receives no peer-disconnected notification. For cleanup
of an already paired operation, follow Section 9.

Such internal failures are not generated in graded cases. The cleanup policy
assumes cleanup/unlock operations themselves succeed; recovery when an unlock
or another cleanup operation also fails is outside the assignment's scope.

Traditional POSIX record locks are associated with a process rather than
with an individual client connection. Therefore, fcntl() alone cannot
distinguish two clients connected to the same server process.

Maintain record_owner[index] for records exclusively owned by an active
update or an acquired transfer operation:
Value 	Meaning
-1 	No local client owns this record
Client fd 	The client that owns this record

Before read, update, or a transfer acquisition attempt accesses a record,
check record_owner[]. If another local client owns it, do not call F_SETLK
to acquire or convert a lock on that record:

    read and update immediately return Locked and remain READY.
    A transfer also immediately returns Locked to its sender. Release any
    partially acquired lock, keep the sender READY, and preserve the receiver's
    WAIT_RECEIVE registration. Do not retain a request or overwrite the owner.

A short-lived read checks record_owner[] but does not modify it. It
acquires and releases its F_RDLCK within one command. An update or acquired
transfer keeps its write lock across commands, so it must store the owning
client fd. For a transfer, both entries store the sender's fd, even though
the receiver supplies the final decision. Receiver registration alone does not own a record; a failed transfer setup
creates no ownership or pairing.

A successful add or a balance-range error must not clear ownership. During
cleanup, clear an ownership entry only if it belongs to the client or
operation being cleaned up. Transfer cleanup initiated by the receiver must
resolve its paired sender and release only that transfer's records. A failed
lock attempt must never clear or unlock another client's record. A normal
read releases only its own temporary read lock and leaves record_owner unchanged.

Example:

Client A (fd 5): update 902001
    record_owner[0] = 5

Client B (fd 6): read 902001
    record_owner[0] belongs to fd 5
    Return Locked without calling F_SETLK.

Client B: read 902002
    record_owner[1] == -1
    Attempt F_RDLCK; if successful, read and release it.
    record_owner[1] stays -1.

The last read can still encounter a lock held by a different server process.
A free local owner entry does not guarantee that the kernel lock is available.

    Same server process: record_owner[] distinguishes clients.
    Different server processes: fcntl() record locks provide protection.

Shared read locks held by different processes are compatible. A write lock
conflicts with another process's read or write lock. Different records remain
independent. Keep the original record_fd open for the server lifetime.
With traditional POSIX record locks, closing any file descriptor in this
process that refers to the same file may release this process's locks on that
file. Merely opening another descriptor does not release them. Do not open
and close accountRecord separately for individual operations.

Concurrency checks hold a client in WAIT_UPDATE. This makes lock conflicts
repeatable without relying on two brief operations happening at the same time.
The public Task 2 uses two server processes, each with one active client, so
it works with the sequential starter. Same-server ownership is assessed under
Task 2 in hidden grading cases; those cases require Task 3's event loop to
handle multiple clients concurrently.
7. TODO 3 / Public Task 3 — Multiplexing and Input (1 point)

Replace the sequential serve_clients() loop with a select() event loop.
Monitor the listener and all connected clients. Use a master fd_set and a
fresh working copy for each select call. Remove disconnected fds from the
master set and reset their request entries.

Maintain a separate state and buffer for every client. A partial command or a
client waiting for its next add must not stall everyone else.

Implement drain_commands() to consume all complete lines in order using
pop_command() and handle_command(). Preserve a partial tail. Evaluate each
line in the state left by the preceding command. For example, a single send:

read 902001
update 902001
add 20
add -5
close
read 902001

With initial balance 500, return six normal responses, in order. The two adds
show 520 and 515 with UPDATE prompts; close has a READY prompt; the last read
shows 515. Close does not stop draining the connection.

exit or invalid input ends that connection: send the terminal response and
discard all later commands in the batch. Retain earlier successful adds.
If a later read line is incomplete, save it and let other clients run.

For Task 4 integration, call close_failed_clients(&master) before select
and skip close_pending clients during processing. Failed-notification cleanup
must not invalidate another peer while its handler is still executing.

There is no timer or lock-retry callback to integrate. Use a NULL timeout in
select; socket activity wakes the loop. Lock acquisition attempts use F_SETLK
and return immediately on conflict. Waiting for a receiver's decision is
ordinary per-client state, so the server continues serving other connections.

Public Task 3 checks concurrent clients, fragmented input, and one send
containing multiple complete commands plus an incomplete tail. Same-server
ownership remains a Task 2 requirement and is checked in hidden grading cases.
EOF/error paths must also remove the socket and release any owned records.
8. TODO 4 / Public Task 4 — Confirmed Transfer with Immediate Lock Failure (2 points)

B registers to receive; A requests a transfer; B accepts before money moves.
Both participants connect to the same server. Other servers share the same
record file and may temporarily hold the records this transfer needs.

Immediate lock failure: if either record is owned by another local client
or either F_SETLK write-lock attempt conflicts, release any acquired lock and
reply Locked to the sender. Do not queue, wait, or automatically retry.
Only after both locks are acquired may the server read, validate, and offer
the transfer. Waiting for acceptance begins only after that successful setup.
8.1 Registration

In READY, B sends receive <target_id>. For receive 902002, reply:

>>> Ready to receive on account 902002.
Waiting for transfer; enter cancel or exit: 

B enters WAIT_RECEIVE. This takes no record lock, and other operations on the
account remain possible. Each account has at most one registered receiver per
server, including a receiver participating in an active offer.
A duplicate registration replies:

">>> [Error] Receiver already registered.\nPlease enter your command: "

The new requester stays READY; the original registration is unchanged.
In WAIT_RECEIVE, cancel removes the registration and replies:

">>> Receive canceled.\nPlease enter your command: "

exit, invalid input, and detected disconnection also remove it. Registration
is local to the server and is only connection matching, not bank authentication.
A receiver on another server cannot be matched. No login is required.
8.2 Request and acquisition

A sends in READY:

transfer <source_id> <target_id> <amount>

Both IDs must be valid and different; amount must be a positive base-10 int.
A leading plus is allowed. Zero, negatives, overflow, and invalid arguments
follow the usual invalid-command-and-close rule.

Apply these checks in order:

    Validate syntax and arguments.
    Find a different registered client in WAIT_RECEIVE on this server. If none
    is available, reply >>> [Error] Receiver unavailable.\n plus READY prompt.
    Attempt the source write lock, then the target write lock, checking both
    local ownership entries before the syscalls. If either conflicts,
    release any first lock and immediately reply Locked to A. Follow the
    failed-setup rules below; do not create a pair or notify B.
    After both locks are held, read both balances and require source balance
    at least amount and target balance plus amount at most MAX_BALANCE. Use
    wide arithmetic. Never cache the balances before acquisition.
    If valid, create an offer and keep both locks through the decision.

No failed acquisition attempt may leave either record owned by this request.
If the second lock fails, release the first before returning. Never release a
lock owned by another operation. Unrelated records remain usable.

If both acquisitions succeed but the balances are invalid, release both
locks and reply only to A:

">>> [Error] Balance out of range.\nPlease enter your command: "

A stays READY, B remains WAIT_RECEIVE, and B receives no notification.
8.3 Lock conflict: fail this request without pairing

On a local-owner conflict or an F_SETLK conflict (EACCES/EAGAIN), send
only to A:

">>> Locked.\nPlease enter your command: "

This is a recoverable operation error, not an invalid-command error:

    A stays READY and its TCP connection remains open.
    Release every lock acquired by this attempt, including the source lock if
    acquiring the target failed. Do not modify either balance.
    Clear temporary transfer fields on A. Do not create or retain a pending pair.
    B remains WAIT_RECEIVE with its registration intact and receives no message.
    B can still cancel its registration or be matched by another valid request.
    Releasing the conflicting lock later does not trigger any notification,
    offer, or transfer. A client must explicitly send a new transfer command.
    A new request performs validation and lock acquisition again, and reads
    current balances only after acquiring both locks.

For example, another server holds an update lock on the target account.
A's transfer immediately replies Locked, leaving B registered. When that
updater later sends close, nothing automatically happens to A or B. A can
choose to send a new transfer request; only this new request may create an offer.

There are no WAIT_TRANSFER_LOCK / WAIT_TRANSFER_PEER states, retry timers,
or a two-second server-side waiting requirement. Do not use F_SETLKW or loop
until a lock becomes available.
8.4 Offer and confirmation

After both locks and valid balances, A enters WAIT_TRANSFER_OUT and receives:

>>> Transfer requested.
Waiting for receiver; enter cancel or exit: 

B enters WAIT_TRANSFER_IN and receives:

>>> Transfer offer: 902001 -> 902002, amount: 100
Please enter accept or reject: 

A successful setup sends these notifications directly. Cross-socket
notification order is not graded, but order within each socket is fixed.

Do not write yet. During WAIT_TRANSFER_OUT/IN, both records are owned by
the sender fd in record_owner[] and are write-locked across processes.
read and update on either account return Locked immediately, including
requests from another server. A client may issue a new command after the
transfer ends; there is no automatic update retry.

Only B may send accept. While retaining both locks, write source minus amount
and target plus amount. Then unlock both, clear registration/pairing, return
both clients to READY, and send the same result to both. For 500 and 1200
transferring 100:

>>> Transfer completed.
>>> Account 902001 balance: 400
>>> Account 902002 balance: 1300
Please enter your command: 

List source first even if its ID is numerically larger. An unrelated or repeated
accept is invalid and cannot apply another transfer.
8.5 Paired cancellation and disconnection

These rules apply to an acquired offer. A failed setup has no pair to cancel:
Action 	Response to each still-connected participant 	Balance change
A cancel 	>>> Transfer canceled.\n + READY prompt 	None
B reject 	>>> Transfer rejected.\n + READY prompt 	None
One participant leaves 	Survivor receives peer-disconnected message below 	None before acceptance

For a departing client's exit, send only >>> Client exit.\n to that client.
For invalid input, send only >>> [Error] Invalid command.\n to that client.
Then close it. Send the survivor:

>>> Transfer canceled: peer disconnected.
Please enter your command: 

EOF, partial-command disconnection, input errors, and failed notifications also
run paired cleanup. Release only locks actually acquired by this pair; cleanup
must never unlock another client's records. Reset both participants and
remove the registration so a reused fd cannot inherit an old request.

Use the supplied close_pending/deferred-close infrastructure when notifying a
peer fails. Clear paired state before sending terminal notifications so cleanup
cannot recursively cancel the same offer twice.

After an acquired offer ends, B must register again for another transfer.
A setup failure preserves B's existing registration, as described above.
A terminal action is processed in event-loop order. If acceptance completes
before EOF is detected, it is not undone. No special priority between truly
simultaneous cancel/accept requests is required.
8.6 State table
State 	Accepted commands 	Held records
WAIT_RECEIVE 	cancel, exit 	None
WAIT_TRANSFER_OUT (A) 	cancel, exit 	Both write-locked, locally owned by A
WAIT_TRANSFER_IN (B) 	accept, reject, exit 	Owned by the paired sender

Other commands in these transfer states are invalid. Transfer decisions must
wait for their notifications. Do not pipeline receive with accept, or an
accept before the offer. At most one pending operation per client is allowed.
8.7 Scope and grading cases

The record format stays unchanged. The starter provides the extra states,
receiver table, request storage, parsers, dispatcher and deferred-close
infrastructure. The acquisition/state/cleanup logic is student work.

No transfer-confirmation timeout or global fairness guarantee is required.
Both participants must use the same server; only record conflicts cross
processes. Use the original record fd throughout the server lifetime.

File I/O is assumed to succeed in graded cases. Two pwrite() calls do not
provide a crash-safe transaction. Disk failure, process termination during the
writes, journaling, and recovery are outside scope. Responsive clients and
small replies are assumed; full nonblocking output queues are not required.

Public cases cover acceptance, rejection/cancellation, either participant's
disconnection, exact nonadjacent record ranges, immediate Locked replies for
local/remote source/target conflicts, rollback of partial acquisitions, and
receiver-registration preservation. Tests also check that releasing a lock
does not resume a failed request; a new request uses the latest balances.
Other tests may combine batching, invalid commands, connection reuse, and
independent pairs.
9. Invalid Input and Cleanup

Unknown commands, wrong-state commands, invalid IDs, missing/extra arguments,
malformed/overflowing integers, and incorrect whitespace are invalid. Reply:

">>> [Error] Invalid command.\n"

Then clean up and disconnect that client. Do not send a READY prompt.
Wrong-state cancel in WAIT_UPDATE is invalid; cleanup still preserves earlier
successful adds. close in READY is invalid. Transfer/receive cancel remains
valid only in the states listed in Section 8.

A syntactically valid add with an out-of-range result stays WAIT_UPDATE
(Section 5.2). An invalid transfer amount is a protocol error; a valid amount
that cannot be covered by the balances is a balance error (Section 8).

EOF may occur while receiving a partial command. Discard that partial input,
release any acquired update/transfer locks, clear matching ownership and
registration, notify a paired survivor when applicable, close the fd, and
reset the request. Cleanup may be invoked through multiple paths and must be
safe when no lock was acquired. A failed transfer attempt must not unlock the client
that caused the conflict.
9.1 Failed response writes

A send_text() failure is treated as a failed client connection. Stop
sending to that client, clean up its active operation, remove the socket from
the monitored set, and close the connection. Do not try to send another error
message or prompt to a connection whose send has already failed.

    For WAIT_UPDATE, release its record lock and matching local owner, clear the
    session, and close the socket. Preserve every successful add already written.
    For WAIT_RECEIVE, remove its receiver registration and close the socket.
    For an active transfer pair, release the pair's two locks, clear matching
    owners/registration and paired state, close the failed connection, and
    notify the still-connected peer with the normal peer-disconnected response.
    A failed setup that never created an offer has no paired peer to notify.
    Likewise, if a transfer already completed or ended before a response fails,
    do not undo it or send a second cancellation for an operation that has ended.

Use deferred-close handling when an immediate reset could invalidate a
request currently being processed. In the supplied structure, a handler can
return false for its current client so the event loop closes it after the
handler returns. When sending to another client fails, mark that client's
close_pending flag and let the supplied cleanup sweep close it safely.
Do not continue dispatching commands for a client marked close_pending.

If notifying the surviving peer also fails, mark that connection for cleanup
as well. Clear the pair before sending cleanup notifications so the same
transfer cannot be recursively canceled twice. Failure of a welcome message
or its initial prompt also closes the newly accepted connection.
10. Public Checker and Grading

make
python3 checker.py --task 1
python3 checker.py --task 2
python3 checker.py --task 3
python3 checker.py --task 4
python3 checker.py --task 1 2 3 4
python3 checker.py

Valid task selectors are 1, 2, 3, 4 only. There are no 3N/3B selectors or
stage3 development build. The same executable server is used throughout.
Public checker results are feedback, not the full weighted final grade.

For each selected task, the checker lists individual case results and a
PASS/FAIL summary. Its final table shows a public-only score estimate:
all public cases in a task must pass for that task's full points to appear in
the estimate; otherwise it shows 0 for that task. This is not per-case partial
credit and is not the official grade. Unselected tasks are marked NOT RUN
and are excluded from the selected-task estimate. Repeated task IDs run once.

The current public suite contains:
Task 	Public cases 	Maximum points in the public-only estimate
1 	2 	1
2 	2 	3
3 	1 	1
4 	6 	2

TAs use additional hidden tests and code review for grading. Passing all
public cases does not guarantee the corresponding official points.
Task 	Points 	Grading focus
1 	1 	Basic commands, immediate and repeated adds, close/exit, no rollback
2 	3 	Exact fcntl lock ranges/types, local ownership, lock lifetime and cleanup
3 	1 	select, per-client state/buffers, fragments and multiple commands
4 	2 	Receiver matching, two-record protection, immediate conflict and cleanup
Total 	7 	

Hidden tests may combine documented behaviors, dynamic balances, boundaries,
wrong-state inputs, terminal commands inside batches, disconnections, and
independent concurrent operations. Same-server ownership is attributed to
Task 2 even when its test needs Task 3's event loop. Passing a public task does
not guarantee full credit. Code review may check use of the required APIs,
absence of busy waiting, and cleanup/resource correctness.

The checker overwrites and restores accountRecord. Never run two checkers or
manual servers against the same directory at the same time.
11. Submission

Submit <student_id>_hw1.zip （ex: b14944222_hw1.zip） to NTU COOL. GitHub Issues is for questions;
GitHub Classroom or pushing a student repository is not required.

Include:

server.c
server.h
Makefile

Include any additional C source/header files required to build your solution.
After extraction, make must generate server without development drivers.
Do not submit binaries, object files, accountRecord, checker.py, private
checkers, TA solutions, core dumps, or Python cache files.

The submission link and designated grading workstation will be provided by
the course announcement on NTU COOL.
11.1 Late Policy

The regular deadline is October 15, 2026, 23:59. Late submissions are
accepted until October 22, 2026, 23:59. All dates and times use
Taiwan time (UTC+8).

Apply the following multiplier to the HW1 score before the late penalty:
Submission date (2026) 	Days late 	Multiplier
October 16-17 	1-2 	0.85
October 18-20 	3-5 	0.70
October 21-22 	6-7 	0.50

Submissions after October 22, 2026, 23:59 are not accepted.

We grade your latest submission on NTU COOL and use its timestamp to
determine the late penalty. Resubmitting after the regular deadline counts
as late, even if you only change comments or formatting. A previous on-time
submission does not exempt a later resubmission from the penalty.
12. FAQ

Does close close the TCP socket?
No. It ends a WAIT_UPDATE session and returns the same client to READY.
Use exit to disconnect normally.

Does close/cancel/disconnection roll back an earlier add?
No. Each successful add is already written. In WAIT_UPDATE, cancel is invalid
and disconnects, but even then earlier successful adds remain.
Transfer cancellation prevents an unaccepted transfer from being applied;
it does not undo unrelated updates already committed by another operation.

Why does add keep its write lock?
The session remains active for further adds. Keeping the lock avoids exposing
the session's account to competing updates until close or connection cleanup.

Is there a blocking update command?
No. update immediately replies Locked if it cannot acquire the record.
An already held session continues until its client closes it or disconnects.

What happens when a transfer cannot acquire its locks?
It immediately replies Locked to the sender, releases any partial acquisition,
and leaves the receiver registered. There is no lock wait or automatic retry.
A client must send a new transfer request if it wants to try again.

Does waiting for accept/reject stop the server?
No. After a successful offer, keep both record locks and per-client state,
then return to select. Other clients continue to use unrelated records.

When are balances read for a transfer?
Only after both locks succeed. Every new transfer request reads the current
balances; never reuse values from an earlier failed request.

Can the two transfer clients connect to different servers?
No. Receiver matching is local. Other servers participate only through shared
record-file locking; they do not exchange transfer notifications.

Does this assignment guarantee crash-safe transfers?
No. Locking coordinates cooperating processes, but two separate record writes
are not a crash-safe transaction. Storage failure recovery is outside scope.
13. Questions and Help

Before contacting TAs, read the specification and FAQ, consult manual pages,
and try searching online or asking an AI tool such as ChatGPT. Use AI to help
understand concepts or diagnose errors; verify its answers against the
specification, documentation, and your own tests, and avoid plagiarism.

If you are still stuck, search existing issues and then post on
GitHub Issues.
Include the command or test, expected and actual behavior, error messages,
and what you have already tried. Do not post your solution code.

    Email: ntusp2026@gmail.com
    TA office hour: Wednesdays, 13:00-14:00, by appointment.

Start early and check HackMD and GitHub Issues regularly for updates.