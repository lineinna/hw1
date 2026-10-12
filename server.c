#define _DEFAULT_SOURCE

#include "server.h"

#include <limits.h>
#include <signal.h>
#include <stdint.h>

/*
 * Implement four TODOs in order:
 *   1. READY commands, complete response writes, update/add/close sessions
 *   2. Nonblocking fcntl record locks and local ownership
 *   3. select multiplexing, fragmented input and batched commands
 *   4. Confirmed two-client transfer and immediate lock failure
 * Socket setup, parsers, record I/O and input buffers are supplied.
 */

static const char *welcome_banner =
    "================================\n"
    " Welcome to CSIE Ledger System \n"
    "================================\n";
static const char *ready_prompt = "Please enter your command: ";
static const char *update_prompt =
    "Please enter add <delta> or close: ";

static const char *receive_prompt = "Waiting for transfer; enter cancel or exit: ";
static const char *transfer_prompt = "Waiting for receiver; enter cancel or exit: ";
static const char *accept_prompt = "Please enter accept or reject: ";

static void cleanup_transfer(request *req);

server svr;
request *requestP;
int maxfd;
int record_fd;
int record_owner[ACCOUNT_NUM];
int receiver_fd[ACCOUNT_NUM];

static int send_text(int fd, const char *text) {
    /*
     * TODO 1: Send the entire string to this blocking client socket using
     * write(). Handle short writes and EINTR. Return 0 on success or -1
     * on failure (including a write that makes no progress).
     *
     * This is also used for the welcome message, so complete it first.
     */
    size_t total = 0;
    size_t len = strlen(text);
    while(total<len){
	ssize_t n = write(fd,text+total,len-total);
	if(n<0){
	    if(errno == EINTR)continue;
	    return -1;
	}
	if(n==0) return -1;
	total += (size_t)n;
    }
    return 0;
}

static void init_request(request *req) {
    memset(req, 0, sizeof(*req));
    req->conn_fd = -1;
    req->peer_fd = -1;
    req->target_index = -1;
    req->state = READY;
    req->account_index = -1;
}

int append_input(request *req) {
    if (req->buf_len >= sizeof(req->buf)) {
        return INPUT_ERROR;
    }
    ssize_t n = read(req->conn_fd, req->buf + req->buf_len,
                     sizeof(req->buf) - req->buf_len);
    if (n == 0) {
        return INPUT_EOF;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return INPUT_RETRY;
        }
        return INPUT_ERROR;
    }
    req->buf_len += (size_t)n;
    return INPUT_DATA;
}

int pop_command(request *req, char *out, size_t out_size) {
    char *newline = memchr(req->buf, '\n', req->buf_len);
    if (newline == NULL) {
        return 0;
    }
    size_t consumed = (size_t)(newline - req->buf) + 1;
    size_t len = consumed - 1;
    if (len > 0 && req->buf[len - 1] == '\r') {
        --len;
    }
    if (len + 1 > out_size) {
        return -1;
    }
    memcpy(out, req->buf, len);
    out[len] = '\0';
    memmove(req->buf, req->buf + consumed, req->buf_len - consumed);
    req->buf_len -= consumed;
    return 1;
}

static int set_record_lock(int index, short type) {
    struct flock lock;
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = (off_t)index*sizeof(account_record);
    lock.l_len = sizeof(account_record);
    return fcntl(record_fd,F_SETLK,&lock);
    /*
     * TODO 2: Apply type to exactly one record using fcntl(F_SETLK).
     * Return the syscall result and preserve errno on failure.
     * The success stub is only for developing Task 1.
     */
    return 0;
}

static void release_update(request *req) {
    if (req->state != WAIT_UPDATE) return;
    set_record_lock(req->account_index,F_UNLCK);
    record_owner[req->account_index] = -1;
    /* TODO 2: Release this request's update lock and matching local owner. */

    /*
     * TODO 1: Reset the update-related fields to their READY values.
     * Preserve the connection and input buffer. Repeated cleanup must be safe.
     */
    req->state = READY;
    req->account_index = -1;
    req->current_balance = 0;
    return;
}

static void close_client(int fd, fd_set *master) {
    request *req = &requestP[fd];
    if (req->state == WAIT_RECEIVE &&
        req->account_index >= 0 && req->account_index < ACCOUNT_NUM &&
        receiver_fd[req->account_index] == fd) {
        receiver_fd[req->account_index] = -1;
    }
    if ((req->state == WAIT_TRANSFER_OUT ||
         req->state == WAIT_TRANSFER_IN) &&
        req->peer_fd >= 0 && req->peer_fd < maxfd) {
        request *peer = &requestP[req->peer_fd];
        if (peer->conn_fd >= 0) {
            if (send_text(peer->conn_fd,
                          ">>> Transfer canceled: peer disconnected.\n"
                          "Please enter your command: ") < 0) {
                peer->close_pending = true;
            }
        }
    }
    cleanup_transfer(&requestP[fd]);
    release_update(&requestP[fd]);
    if(master!=NULL) FD_CLR(fd,master);
    /*
     * TODO 3: Remove fd from the monitored set when master is non-NULL.
     * The sequential development loop passes NULL.
     */
//    (void)master;

    close(fd);
    init_request(&requestP[fd]);
}

/* Supplied infrastructure: close failed peer notifications outside handlers.
 * Call before select(), and skip close_pending clients in the readable batch.
 * This drains known cleanup work; it does not poll for network events.
 */
static void close_failed_clients(fd_set *master) {
    bool again;
    do {
        again = false;
        for (int fd = 0; fd < maxfd; ++fd) {
            if (requestP[fd].conn_fd >= 0 && requestP[fd].close_pending) {
                close_client(fd, master);
                again = true;
            }
        }
    } while (again);
}

static bool parse_int32(const char *text, int *value) {
    const char *digits = text;
    if (*digits == '+' || *digits == '-') ++digits;
    if (*digits == '\0') return false;
    for (const char *p = digits; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    if (*text == '\0' || *text == ' ' || *text == '\t') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool parse_account_command(const char *line, const char *verb,
                                  int *index) {
    size_t verb_len = strlen(verb);
    if (strncmp(line, verb, verb_len) != 0 || line[verb_len] != ' ') {
        return false;
    }
    const char *arg = line + verb_len + 1;
    if (*arg == '\0' || strchr(arg, ' ') != NULL || strchr(arg, '\t') != NULL) {
        return false;
    }
    int id;
    if (!parse_int32(arg, &id) ||
        id < ACCOUNT_ID_START || id > ACCOUNT_ID_END) {
        return false;
    }
    *index = id - ACCOUNT_ID_START;
    return true;
}



static bool parse_delta_command(const char *line, int *delta) {
    const char prefix[] = "add ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *arg = line + sizeof(prefix) - 1;
    if (*arg == '\0' || strchr(arg, ' ') != NULL || strchr(arg, '\t') != NULL) {
        return false;
    }
    return parse_int32(arg, delta);
}

static bool parse_transfer_command(const char *line, int *source,
                                   int *target, int *amount) {
    const char prefix[] = "transfer ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;
    char args[MAX_MSG_LEN];
    if (strlen(line) >= sizeof(args)) return false;
    strcpy(args, line + sizeof(prefix) - 1);
    char *second = strchr(args, ' ');
    if (second == NULL) return false;
    *second++ = '\0';
    char *third = strchr(second, ' ');
    if (third == NULL) return false;
    *third++ = '\0';
    int src, dst;
    if (!parse_int32(args, &src) || !parse_int32(second, &dst) ||
        !parse_int32(third, amount) || *amount <= 0 || src == dst ||
        src < ACCOUNT_ID_START || src > ACCOUNT_ID_END ||
        dst < ACCOUNT_ID_START || dst > ACCOUNT_ID_END) return false;
    *source = src - ACCOUNT_ID_START;
    *target = dst - ACCOUNT_ID_START;
    return true;
}

static int read_record_at(int index, account_record *record) {
    off_t offset = (off_t)index * (off_t)sizeof(*record);
    return pread(record_fd, record, sizeof(*record), offset) ==
                   (ssize_t)sizeof(*record)
               ? 0
               : -1;
}

static int write_record_at(int index, const account_record *record) {
    off_t offset = (off_t)index * (off_t)sizeof(*record);
    return pwrite(record_fd, record, sizeof(*record), offset) ==
                   (ssize_t)sizeof(*record)
               ? 0
               : -1;
}

/* A handler returns true to keep the connection, false to close it. */
static bool handle_ready(request *req, const char *line) {
    int index;
    char response[MAX_MSG_LEN];
    account_record record;
    if(strcmp("exit",line)==0){
        snprintf(response,sizeof(response),
            ">>> Client exit.\n");
        send_text(req->conn_fd,response);
        return false;
    }
    if(parse_account_command(line,"read",&index)){
        if(record_owner[index]!=-1){
            snprintf(response,sizeof(response),
                ">>> Locked.\n%s",ready_prompt);
            return send_text(req->conn_fd,response) == 0;
        }
        int result = set_record_lock(index,F_RDLCK);
        if(result<0){
            if(errno==EAGAIN||errno==EACCES){
                snprintf(response,sizeof(response),
                    ">>> Locked.\n%s",ready_prompt);
                return send_text(req->conn_fd,response) == 0;
            }
            else return false;
        }
        if(read_record_at(index,&record)<0){
            set_record_lock(index,F_UNLCK); 
            return false;
        }
        snprintf(response,sizeof(response),
            ">>> Account %d balance: %d\n%s",
            record.id,
            record.balance,
            ready_prompt);
        if(set_record_lock(index,F_UNLCK)<0) return false;
        return send_text(req->conn_fd,response) == 0;
    }else if(parse_account_command(line,"update",&index)){
        if(record_owner[index]!=-1){
            snprintf(response,sizeof(response),
                ">>> Locked.\n%s",ready_prompt);
            return send_text(req->conn_fd,response) == 0;
        }
        int result = set_record_lock(index,F_WRLCK);
        if(result<0){
            if(errno==EAGAIN||errno==EACCES){
                snprintf(response,sizeof(response),
                    ">>> Locked.\n%s",ready_prompt);
                return send_text(req->conn_fd,response) == 0;
            }
            else return false;
        }
        record_owner[index] = req->conn_fd;
        if(read_record_at(index,&record)<0){
            set_record_lock(index,F_UNLCK); 
            record_owner[index] = -1;
            return false;
        }
        req->state = WAIT_UPDATE;
        req->account_index = index;
        req->current_balance = record.balance;
        snprintf(response,sizeof(response),
            ">>> Account %d balance: %d\n"
            ">>> Update lock acquired.\n%s",
            record.id,
            record.balance,
            update_prompt);
        return send_text(req->conn_fd,response) == 0;
    }else{
        snprintf(response,sizeof(response),
            ">>> [Error] Invalid command.\n");
        send_text(req->conn_fd,response);
        return false;
    }
    /* TODO 1: read <id>, update <id>, exit. Invalid input closes the client.
     * TODO 2: read takes a short shared lock; update retains a write lock.
     * Check local ownership too. A lock conflict immediately replies Locked.
     * After acquiring an update lock, read the current balance and enter
     * WAIT_UPDATE. There is no blocking update mode or retry state.
     */
    (void)req; (void)line;
    (void)ready_prompt; (void)update_prompt;
    (void)parse_account_command; (void)read_record_at; (void)set_record_lock;
    return false;
}

static bool handle_wait_update(request *req, const char *line) {
    int index = req->account_index;
    char response[MAX_MSG_LEN];
    account_record record;
    if(strcmp(line,"close")==0){
        release_update(req);
        snprintf(response,sizeof(response),
        ">>> Update closed.\n%s",
        ready_prompt);
        return send_text(req->conn_fd,response) == 0;
    }else if(strcmp(line,"exit")==0){
        release_update(req);
        snprintf(response,sizeof(response),
            ">>> Client exit.\n");
        send_text(req->conn_fd,response);
        return false;
    }
    int dt;
    if(parse_delta_command(line, &dt)==true){
        if(read_record_at(index,&record)<0) return false;
        if((long long)record.balance+(long long)dt<0
            ||(long long)record.balance+(long long)dt>MAX_BALANCE){
            snprintf(response,sizeof(response),
            ">>> [Error] Balance out of range.\n%s",
            update_prompt);
            return send_text(req->conn_fd,response) == 0;
        }
        record.balance += dt;
        if(write_record_at(index,&record)<0) return false;
        req->current_balance = record.balance;
        snprintf(response,sizeof(response),
            ">>> Update successful.\n"
            ">>> Account %d balance: %d\n%s",
            record.id,
            record.balance,
            update_prompt);
        return send_text(req->conn_fd,response) == 0;
    }else{
        snprintf(response,sizeof(response),
            ">>> [Error] Invalid command.\n");
        send_text(req->conn_fd,response);
        return false;
    }
    /* TODO 1: add <delta>, close, exit.
     * Each valid add writes immediately and updates current_balance; remain
     * in WAIT_UPDATE, retaining the lock. A range error also stays here.
     * close releases this session and returns READY without closing the socket.
     * exit/invalid input/EOF clean up and close the socket. Never roll back
     * previously successful adds. cancel is invalid in WAIT_UPDATE.
     * TODO 2: only close/connection cleanup releases the update lock/owner.
     */
    (void)req; (void)line;
    (void)parse_delta_command; (void)write_record_at;
    return false;
}

/* TODO 4: Complete the two-client transfer lifecycle described in SPEC.md.
 * receiver_fd[] is registration only; it must not replace record_owner[].
 * Both record_owner entries of a pending offer belong to the sender fd.
 * Use the sender request to store the two indices, balances, amount and peer.
 * Set peer.close_pending if a peer notification fails; do not close a peer
 * inside a command handler. The supplied sweep handles those failures.
 */
static void reset_transfer_fields(request *req)
{
    req->state = READY;
    req->account_index = -1;
    req->current_balance = 0;
    req->peer_fd = -1;
    req->target_index = -1;
    req->target_balance = 0;
    req->amount = 0;
}
static void cleanup_transfer(request *req) {
    request *sender;
    request *receiver;
    int source;
    int target;
    int sender_fd;

    if (req->state == WAIT_TRANSFER_OUT) {
        sender = req;
        if (req->peer_fd < 0 || req->peer_fd >= maxfd)
            return;
        receiver = &requestP[req->peer_fd];
    } else if (req->state == WAIT_TRANSFER_IN) {
        receiver = req;
        if (req->peer_fd < 0 || req->peer_fd >= maxfd)
            return;
        sender = &requestP[req->peer_fd];
    } else {
        return;
    }

    if (sender->state != WAIT_TRANSFER_OUT ||
        sender->peer_fd != receiver->conn_fd ||
        receiver->peer_fd != sender->conn_fd) {
        return;
    }

    source = sender->account_index;
    target = sender->target_index;
    sender_fd = sender->conn_fd;

    if (source >= 0 && source < ACCOUNT_NUM &&
        record_owner[source] == sender_fd) {
        set_record_lock(source, F_UNLCK);
        record_owner[source] = -1;
    }
    if (target >= 0 && target < ACCOUNT_NUM &&
        record_owner[target] == sender_fd) {
        set_record_lock(target, F_UNLCK);
        record_owner[target] = -1;
    }
    if (target >= 0 && target < ACCOUNT_NUM &&
        receiver_fd[target] == receiver->conn_fd) {
        receiver_fd[target] = -1;
    }

    reset_transfer_fields(sender);
    reset_transfer_fields(receiver);
}

static bool handle_transfer_ready(request *req, const char *line) {
    char response[MAX_MSG_LEN];
    int index;
    int src,dst,amount;
    if(parse_account_command(line,"receive",&index)){
        if(receiver_fd[index]!=-1){
            snprintf(response,sizeof(response),
            ">>> [Error] Receiver already registered.\n%s",
                ready_prompt);
            return send_text(req->conn_fd,response) == 0;
        }
        receiver_fd[index]  = req->conn_fd;
        req->state = WAIT_RECEIVE;
        req->account_index = index;
//        req->current_balance = record.balance;
        snprintf(response,sizeof(response),
            ">>> Ready to receive on account %d.\n%s",
            index+ACCOUNT_ID_START,
            receive_prompt);
        if(send_text(req->conn_fd,response)<0){
            receiver_fd[index]  = -1;
            req->state = READY;
            req->account_index = -1;
            return false;
        }
        return true;
    }else if(parse_transfer_command(line,&src,&dst,&amount)==true){
        if(receiver_fd[dst] == -1){
            snprintf(response,sizeof(response),
            ">>> [Error] Receiver unavailable.\n%s",
                ready_prompt);
            return send_text(req->conn_fd,response) == 0;
        }
        int dst_fd = receiver_fd[dst];
        int src_fd = req->conn_fd;
        if((record_owner[src]!=-1
            &&record_owner[src]!=src_fd)){
            snprintf(response,sizeof(response),
                ">>> Locked.\n%s", ready_prompt);
            return send_text(req->conn_fd,response)==0;
        }
        int result = set_record_lock(src,F_WRLCK);
        if(result<0){
            if(errno==EAGAIN||errno==EACCES){
                snprintf(response,sizeof(response),
                    ">>> Locked.\n%s",ready_prompt);
                return send_text(req->conn_fd,response) == 0;
            }
            else return false;
        }
        record_owner[src] = src_fd;
        if((record_owner[dst]!=-1
            &&record_owner[dst]!=src_fd)){
            snprintf(response,sizeof(response),
                ">>> Locked.\n%s", ready_prompt);
            record_owner[src] = -1;
            set_record_lock(src,F_UNLCK);
            return send_text(req->conn_fd,response)==0;
        }
        result = set_record_lock(dst,F_WRLCK);
        if(result<0){
            if(errno==EAGAIN||errno==EACCES){
                record_owner[src] = -1;
                snprintf(response,sizeof(response),
                    ">>> Locked.\n%s",ready_prompt);  
                set_record_lock(src,F_UNLCK);
                return send_text(req->conn_fd,response)==0;
            }
            set_record_lock(src,F_UNLCK);
            record_owner[src] = -1;
            return false;
        }
        record_owner[dst] = src_fd;
        account_record src_record;
        account_record dst_record;
        if(read_record_at(src,&src_record)<0
            ||read_record_at(dst,&dst_record)<0){
            record_owner[src] = -1;
            record_owner[dst] = -1;
            set_record_lock(src,F_UNLCK);
            set_record_lock(dst,F_UNLCK);
            return false;
        }
        long long newsrcbalance = (long long)src_record.balance;
        newsrcbalance-=amount;
        long long newdstbalance = (long long)dst_record.balance;
        newdstbalance += amount;
        if(newsrcbalance<0||newdstbalance>MAX_BALANCE){
            snprintf(response,sizeof(response),
                ">>> [Error] Balance out of range.\n%s",
                ready_prompt);
            record_owner[src] = -1;
            record_owner[dst] = -1;
            set_record_lock(src,F_UNLCK);
            set_record_lock(dst,F_UNLCK);
            return send_text(req->conn_fd,response)==0;
        }
        req->state = WAIT_TRANSFER_OUT;
        req->account_index = src;
        req->current_balance = src_record.balance;
        snprintf(response,sizeof(response),
            ">>> Transfer requested.\n%s",
            transfer_prompt);
        req->state = WAIT_TRANSFER_OUT;
        req->account_index = src;
        req->target_index = dst;
        req->amount = amount;
        req->peer_fd = dst_fd;
        requestP[dst_fd].state = WAIT_TRANSFER_IN;
        requestP[dst_fd].peer_fd = src_fd;
        int src_id = ACCOUNT_ID_START+src;
        int dst_id = ACCOUNT_ID_START+dst;
        if(send_text(req->conn_fd,response)<0){
            cleanup_transfer(req);
            return false;
        }
        snprintf(response, sizeof(response), 
            ">>> Transfer offer: %d -> %d, amount: %d\n%s", 
            src_id, dst_id, amount, accept_prompt);
        if(send_text(dst_fd,response)<0){
            cleanup_transfer(req);
            return false;
        }
        return true;
    }else{
        snprintf(response,sizeof(response),
            ">>> [Error] Invalid command.\n");
        send_text(req->conn_fd,response);
        return false;
    }
    /* TODO 4: receive <id>, transfer <source> <target> <amount>.
     * Match a local receiver, check local owners, and try both F_WRLCK locks.
     * Any conflict: release acquired locks, reply Locked to the sender,
     * leave it READY, and preserve the receiver's WAIT_RECEIVE registration.
     * Never queue or automatically retry a failed transfer.
     * Only after BOTH locks succeed, read/check balances and create an offer.
     * Balance failure also releases both locks and preserves registration.
     */
    (void)req; (void)line;
    (void)parse_transfer_command;
    (void)receive_prompt; (void)transfer_prompt; (void)accept_prompt;
    return false;
}

static bool handle_wait_transfer(request *req, const char *line) {
    char response[MAX_MSG_LEN];
    if(req->state == WAIT_RECEIVE){
        if(strcmp(line,"cancel")==0){
            receiver_fd[req->account_index]  = -1;
            req->state = READY;
            req->account_index = -1;
            req->current_balance = 0;
            snprintf(response,sizeof(response),
                ">>> Receive canceled.\n%s",
                ready_prompt);
            return send_text(req->conn_fd,response) == 0;
        }else if(strcmp(line,"exit")==0){
            receiver_fd[req->account_index]  = -1;
            req->state = READY;
            req->account_index = -1;
            req->current_balance = 0;
            snprintf(response, sizeof(response),
                ">>> Client exit.\n");
            send_text(req->conn_fd,response);
            return false;
        }else{
            receiver_fd[req->account_index]  = -1;
            req->state = READY;
            req->account_index = -1;
            req->current_balance = 0;
            snprintf(response, sizeof(response),    
                ">>> [Error] Invalid command.\n");
            send_text(req->conn_fd, response);
            return false;
        }
        return true;
    }
    if(req->state == WAIT_TRANSFER_OUT){
        if(strcmp(line,"cancel")==0){
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> Transfer canceled.\n%s",
                ready_prompt);
            int rs = send_text(req->conn_fd,response);
            if(peer!=NULL){
                snprintf(response,sizeof(response),
                    ">>> Transfer canceled.\n%s",
                    ready_prompt);
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            if(rs<0) return false;
            return true;
        }else if(strcmp(line,"exit")==0){
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> Client exit.\n");
            send_text(req->conn_fd,response);
            if(peer!=NULL){
                snprintf(response,sizeof(response),
                    ">>> Transfer canceled: peer disconnected.\n%s",
                    ready_prompt);
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            return false;
        }else{
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> [Error] Invalid command.\n%s",
                ready_prompt);
            send_text(req->conn_fd,response);
            if(peer!=NULL){
                snprintf(response,sizeof(response),
                    ">>> Transfer canceled: peer disconnected.\n%s",
                    ready_prompt);
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            return false;
        }
    }
    if(req->state == WAIT_TRANSFER_IN){
        if(strcmp(line,"accept")==0){
            request *sender = &requestP[req->peer_fd];
            int src = sender->account_index;
            int dst = sender->target_index;
            int amount = sender->amount;
            account_record src_record;
            account_record dst_record;
            if(read_record_at(src,&src_record)<0
                ||read_record_at(dst,&dst_record)<0){
                cleanup_transfer(req);
                return false;
            }
            src_record.balance -= amount;
            dst_record.balance += amount;
            if(write_record_at(src,&src_record)<0
                ||write_record_at(dst,&dst_record)<0){
                cleanup_transfer(req);
                return false;
            }
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> Transfer completed.\n"
                ">>> Account %d balance: %d\n"
                ">>> Account %d balance: %d\n%s",
                ACCOUNT_ID_START + src, src_record.balance,
                ACCOUNT_ID_START + dst, dst_record.balance,
                ready_prompt);
            int rs = send_text(req->conn_fd,response);
            if(peer!=NULL){
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            if(rs<0) return false;
            return true;
        }else if(strcmp(line,"reject")==0){
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> Transfer rejected.\n%s",
                ready_prompt);
            int rs = send_text(req->conn_fd,response);
            if(peer!=NULL){
                snprintf(response,sizeof(response),
                    ">>> Transfer rejected.\n%s",
                    ready_prompt);
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            if(rs<0) return false;
            return true;
        }else if(strcmp(line,"exit")==0){
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> Client exit.\n");
            send_text(req->conn_fd,response);
            if(peer!=NULL){
                snprintf(response,sizeof(response),
                    ">>> Transfer canceled: peer disconnected.\n%s",
                    ready_prompt);
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            return false;
        }else{
            request *peer = NULL;
            if(req->peer_fd >= 0 && req->peer_fd < maxfd){
                peer = &requestP[req->peer_fd];
            }
            snprintf(response,sizeof(response),
                ">>> [Error] Invalid command.\n%s",
                ready_prompt);
            send_text(req->conn_fd,response);
            if(peer!=NULL){
                snprintf(response,sizeof(response),
                    ">>> Transfer canceled: peer disconnected.\n%s",
                    ready_prompt);
                int ps = send_text(peer->conn_fd,response);
                if(ps<0) peer->close_pending = true;
            }
            cleanup_transfer(req);
            return false;
        }
    }
    /* TODO 4: Handle receive and transfer-confirmation states.
     * Only the matched receiver may accept/reject; only the sender cancels
     * a pending offer. Exit/invalid input use close_client() cleanup.
     * Complete/cancel/reject must release both locks and registration.
     */
    (void)line;
    (void)send_text(req->conn_fd, ">>> [Error] Invalid command.\n");
    return false;
}

/* Supplied per-client state dispatcher. */
static bool handle_command(request *req, const char *line) {
    if (req->state == READY) {
        if (strncmp(line, "receive ", 8) == 0 ||
            strncmp(line, "transfer ", 9) == 0) {
            return handle_transfer_ready(req, line);
        }
        return handle_ready(req, line);
    }
    if (req->state == WAIT_UPDATE)
        return handle_wait_update(req, line);
    return handle_wait_transfer(req, line);
}

static bool drain_commands(request *req){
    bool keep = true;
    int fd = req->conn_fd;
    while (keep) {
        char line[MAX_MSG_LEN];
        int popped = pop_command(&requestP[fd], line, sizeof(line));
        if (popped == 0) {
            return true;
        }
        if (popped < 0) {
            (void)send_text(fd, ">>> [Error] Invalid command.\n");
            return false;
        }
        keep = handle_command(&requestP[fd], line);
    }
    return false;
}
    /* TODO 3: dispatch ALL complete lines in order and preserve a partial tail.
     * Stop and discard remaining input when a handler requests disconnection.
     * add remains WAIT_UPDATE; close returns READY, so later lines in the
     * same batch must be interpreted using the resulting state.
     */
//  (void)req; (void)handle_command;
//  return true;

static void init_server(unsigned short port) {
    struct sockaddr_in addr;
    int reuse = 1;
    gethostname(svr.hostname, sizeof(svr.hostname));
    svr.port = port;
    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) {
        ERR_EXIT("socket");
    }
    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        ERR_EXIT("setsockopt");
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(svr.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }
    maxfd = FD_SETSIZE;
    requestP = calloc((size_t)maxfd, sizeof(*requestP));
    if (requestP == NULL) {
        ERR_EXIT("calloc");
    }
    for (int i = 0; i < maxfd; ++i) {
        init_request(&requestP[i]);
    }
    for (int i = 0; i < ACCOUNT_NUM; ++i) {
        record_owner[i] = -1;
        receiver_fd[i] = -1;
    }
}


static void serve_clients(void) {
    fd_set master_set,working_set;
    int max_nfds = svr.listen_fd;
    FD_ZERO(&master_set);
    FD_SET(svr.listen_fd,&master_set);
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    while(1){
        close_failed_clients(&master_set);
        memcpy(&working_set,&master_set,sizeof(master_set));
        peer_len = sizeof(peer);
        if(select(max_nfds+1,&working_set,NULL,NULL,NULL)<0) continue;
        for(int i=0;i<=max_nfds;i++){
            if(FD_ISSET(i,&working_set)!=true) continue;
            if(requestP[i].close_pending) continue;
            if(i == svr.listen_fd){
                int fd = accept(svr.listen_fd, 
                    (struct sockaddr *)&peer, &peer_len);
                if (fd < 0) {
                    if (errno == EINTR) continue;
                    ERR_EXIT("accept");
                }
                if (fd >= maxfd) {
                    close(fd);
                    continue;
                }
                FD_SET(fd,&master_set);
                if(fd>max_nfds) max_nfds = fd;
                init_request(&requestP[fd]);
                requestP[fd].conn_fd = fd;
                if (send_text(fd, welcome_banner) < 0 ||
                    send_text(fd, ready_prompt) < 0) {
                    close_client(fd, &master_set);
                    if(fd == max_nfds){
                        for(int idx=max_nfds-1;idx>=0;idx--){
                            if(FD_ISSET(idx,&master_set)==true){
                                max_nfds = idx;
                                break;
                            }
                        }
                    }
                    continue;
                }
            }else{
                int status = append_input(&requestP[i]);
                if (status == INPUT_EOF || status == INPUT_ERROR){
                    close_client(i, &master_set);
                    if(i == max_nfds){
                        for(int idx=max_nfds-1;idx>=0;idx--){
                            if(FD_ISSET(idx,&master_set)==true){
                                max_nfds = idx;
                                break;
                            }
                        }
                    }
                    continue;
                }
                if (status == INPUT_RETRY) continue;
                else{
                    int res = drain_commands(requestP+i);
                    if(res==false){
                        if(i == max_nfds){
                            for(int idx=max_nfds-1;idx>=0;idx--){
                                if(FD_ISSET(idx,&master_set)==true){
                                    max_nfds = idx;
                                    break;
                                }
                            }
                        }
                        close_client(i,&master_set);
                    } 
                }
            }
        }
    }
    (void)drain_commands; /* Used by the TODO 3 event loop. */
    /*
     * TODO 3: Replace this one-client-at-a-time loop with select().
     * Keep independent client states and partial input, and handle cleanup.
     * No lock-retry timer is needed; select may use a NULL timeout.
     * Call close_failed_clients(&master) before select(), skip close_pending
     * clients in a readable batch, and use drain_commands() after input.
     * The supplied sequential loop supports Tasks 1 and 2 development.
     * Implement this loop for Tasks 3 and 4. Transfer lock conflicts return
     * Locked immediately; no waiting states or automatic retries exist.
     */
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    record_fd = open(RECORD_PATH, O_RDWR);
    if (record_fd < 0) {
        ERR_EXIT("open accountRecord");
    }
    init_server((unsigned short)atoi(argv[1]));
    serve_clients();
    return EXIT_SUCCESS;
}
