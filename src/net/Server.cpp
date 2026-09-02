#include "amlp/net/Server.hpp"
#include "amlp/config/Config.hpp"
#include "amlp/vm/VM.hpp"
#include "amlp/object/ObjectManager.hpp"
#include "amlp/object/LpcObject.hpp"
#include "amlp/net/OutputContext.hpp"
#include "amlp/net/SocketRegistry.hpp"
#include "amlp/net/SnoopRelay.hpp"
#include "amlp/core/Errors.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <algorithm>

namespace amlp {

namespace {
bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// string|function dispatch shared by every socket callback -- the exact
// same two-shape Value dispatchLine() already uses just below for
// notify_fail()'s own pending message/closure. A destructed owner
// (weak_ptr lock() failure) silently drops the callback rather than
// erroring, matching PendingInputTo's own established O_DESTRUCTED
// handling (see LpcSocket.hpp's own comment on LpcSocket::owner).
void fireSocketCallback(VM& vm, const Value& callback, const std::weak_ptr<LpcObject>& owner,
                         std::vector<Value> args) {
    if (std::holds_alternative<std::monostate>(callback.data)) return;
    if (auto* closure = std::get_if<std::shared_ptr<Closure>>(&callback.data)) {
        if (!*closure) return;
        try {
            vm.callClosure(*closure, std::move(args));
        } catch (const std::exception& e) {
            std::cerr << "[net] socket closure callback failed: " << e.what() << "\n";
        }
        return;
    }
    if (auto* name = std::get_if<std::string>(&callback.data)) {
        auto ob = owner.lock();
        if (!ob) return;
        try {
            // Real socket_efuns.c's own read/close/write callback firing:
            // "safe_apply(callback.s, lpc_socks[fd].owner_ob, num_arg,
            // ORIGIN_INTERNAL)", confirmed directly (both real call sites,
            // not just one) -- the same real origin call_out()'s own
            // string-form firing uses (see Scheduler.cpp's own comment),
            // not ORIGIN_DRIVER despite both being driver-triggered.
            vm.callFunction(ob, *name, std::move(args), Origin::Internal);
        } catch (const std::exception& e) {
            std::cerr << "[net] socket callback " << *name << "() failed: " << e.what() << "\n";
        }
    }
}

// Real "flags |= S_LINKDEAD; socket_close(fd, SC_FORCE | SC_DO_CALLBACK |
// SC_FINAL_CLOSE);" -- the driver-detected side of a socket going away
// (peer EOF, a read/write error), as opposed to the LPC-initiated
// SocketRegistry::close() path, which never fires close_callback (see
// SocketRegistry.hpp's own comment on why). sock is a local shared_ptr
// copy from SocketRegistry::all(), so it stays valid for the fire below
// even after SocketRegistry::forceRemove() erases the registry's own
// entry.
void closeSocketAndFireCallback(VM& vm, const std::shared_ptr<LpcSocket>& sock) {
    Value cb = sock->closeCallback;
    std::weak_ptr<LpcObject> owner = sock->owner;
    int handle = sock->handle;
    SocketRegistry::forceRemove(handle);
    fireSocketCallback(vm, cb, owner, {Value(static_cast<int64_t>(handle))});
}
}

Server::Server(Config& config, VM& vm, ObjectManager& objects, Scheduler& scheduler)
    : config_(config), vm_(vm), objects_(objects), scheduler_(scheduler) {}

Server::~Server() {
    if (listenFd_ >= 0) {
        ::close(listenFd_);
    }
}

bool Server::listen() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "[net] socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(config_.port()));

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[net] bind() failed on port " << config_.port()
                   << ": " << std::strerror(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (::listen(listenFd_, 16) < 0) {
        std::cerr << "[net] listen() failed: " << std::strerror(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (!setNonBlocking(listenFd_)) {
        std::cerr << "[net] warning: failed to set listen socket non-blocking\n";
    }

    std::cout << "[net] listening on port " << config_.port() << "\n";
    return true;
}

void Server::onNewConnection(int clientFd) {
    setNonBlocking(clientFd);
    int one = 1;
    ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    auto conn = std::make_shared<Connection>(clientFd);

    // Real new_user() (comm.c): "if (external_port[which].kind ==
    // PORT_TELNET) { add_binary_message(ob, telnet_do_ttype, ...);
    // add_binary_message(ob, telnet_do_naws, ...); ...
    // add_binary_message(ob, telnet_do_mxp, ...); }" fires right at
    // connection setup, unprompted, before master->connect() even runs --
    // TTYPE first, then NAWS, in that exact real order (row 3.4's own
    // slice: TTYPE negotiation, matched here; MXP stays unsent, this
    // driver does not process a client's WILL MXP response yet).
    conn->send(std::string("\xff\xfd\x18", 3));  // IAC DO TTYPE (255 253 24)
    conn->send(std::string("\xff\xfd\x1f", 3));  // IAC DO NAWS (255 253 31)

    OutputContext::set(conn.get());

    // A runtime error out of master->connect() (a bad clone_object(), a
    // missing efun somewhere in the login chain, etc) must fail *this*
    // connection attempt, not the whole driver process -- the same
    // "one object's runtime error is not a process crash" guarantee
    // ObjectManager::loadObject()/cloneObject() already give create(),
    // just not previously extended to this call site. Without this, one
    // player hitting a bug in the login flow silently disconnects every
    // other player on the mud too (confirmed live: attempting
    // /secure/std/login.c before it actually compiled took the whole
    // process down on the very first connection).
    Value result;
    try {
        result = vm_.applyMaster("connect", {});
    } catch (const std::exception& e) {
        OutputContext::set(nullptr);
        std::cerr << "[net] master->connect() failed: " << e.what() << "\n";
        ::close(clientFd);
        return;
    }

    if (!std::holds_alternative<std::shared_ptr<LpcObject>>(result.data)) {
        OutputContext::set(nullptr);
        std::cerr << "[net] master->connect() did not return an object; "
                      "closing connection\n";
        ::close(clientFd);
        return;
    }

    auto loginObj = std::get<std::shared_ptr<LpcObject>>(result.data);
    if (!loginObj) {
        OutputContext::set(nullptr);
        std::cerr << "[net] master->connect() returned a null object\n";
        ::close(clientFd);
        return;
    }

    conn->attach(loginObj);
    std::cout << "[net] connection fd=" << clientFd
               << " bound to " << loginObj->filename() << "\n";

    // Real FluffOS calls logon() on the freshly bound object immediately
    // after binding, before anything else touches the connection --
    // backend.c's logon(): "apply(APPLY_LOGON, ob, 0, ORIGIN_DRIVER);"
    // (zero arguments), invoked from comm.c's new_user_handler() right
    // after "ob->interactive = master_ob->interactive;" and friends. A
    // missing logon() is not an error ("function not existing is no
    // longer fatal" -- backend.c's own comment); findFunctionInChain
    // already makes VM::callFunction() silently return void in that
    // case, so no special-casing is needed here for that part.
    //
    // A *runtime* error inside a defined logon() follows the same
    // per-connection failure isolation as master->connect() above and
    // the per-line dispatch below: it closes only this connection, not
    // the whole driver.
    try {
        vm_.callFunction(loginObj, "logon", {});
    } catch (const std::exception& e) {
        OutputContext::set(nullptr);
        std::cerr << "[net] connection fd=" << clientFd
                   << " logon() failed: " << e.what() << "\n";
        conn->close();
        return;
    }

    OutputContext::set(nullptr);
    connections_.push_back(std::move(conn));
}

void Server::acceptNewConnections() {
    for (;;) {
        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            std::cerr << "[net] accept() failed: " << std::strerror(errno) << "\n";
            break;
        }
        onNewConnection(clientFd);
    }
}

// Real FluffOS's process_user_command() (comm.c): a pending input_to()
// registration always takes the next raw line ahead of anything else --
// "if (call_function_interactive(ip, user_command)) { goto exit; } ...
// process_input(ip, user_command);" -- call_function_interactive() is
// checked first and, if it consumed the line (a handler was pending),
// process_input() is skipped entirely for that line. Only when nothing
// is pending does process_input() run instead.
void Server::dispatchLine(VM& vm, Connection& conn, const std::string& line) {
    // Real get_user_command() (comm.c): "ip->last_time = current_time"
    // runs the moment a full command line is pulled off the buffer, for
    // every line -- including one a pending input_to() handler is about
    // to consume -- and before process_user_command() itself even runs.
    // query_idle() (EfunTable.cpp) reads this back.
    conn.touchActivity();

    // Real process_user_command() (comm.c): clear_notify(ip->ob) runs
    // unconditionally at the very top, before even checking for a
    // pending input_to() handler -- a notify_fail() message set during
    // an earlier, unrelated dispatch must never leak into this one.
    conn.clearPendingNotifyFail();

    // Real interpret.c/backend.c: eval_cost is reset to 0 once at the
    // start of each top-level dispatch (process_user_command /
    // call_heart_beat / call_call_out) and then accumulates across all
    // nested apply/call_other/callClosure calls. Matches the single
    // global reset in the reference driver, not per-run() reset.
    vm.resetEvalCost();

    if (conn.hasPendingInputTo()) {
        std::optional<PendingInputTo> pending = conn.takePendingInputTo();
        auto target = pending->object.lock();
        // Real FluffOS's own O_DESTRUCTED check in
        // call_function_interactive(): a handler whose object died
        // before its next line arrived is simply dropped, not an error.
        if (target) {
            std::vector<Value> callArgs;
            callArgs.reserve(1 + pending->extraArgs.size());
            callArgs.emplace_back(line);
            for (auto& extra : pending->extraArgs) callArgs.push_back(extra);
            // Real comm.c's own call_function_interactive(): "apply(function,
            // ob, num_arg + 1, ORIGIN_INTERNAL)" for the string-name form,
            // confirmed directly (the closure form there calls
            // call_function_pointer() instead, exactly what
            // VM::callClosure()'s own tiered resolution already handles for
            // free, no explicit origin needed at this driver's own
            // equivalent -- notify_no_command()'s function-form dispatch
            // just below in this same file works the same way).
            vm.callFunction(target, pending->function, std::move(callArgs), Origin::Internal);
        }
        return;
    }

    // comm.c's process_input() (static void process_input(interactive_t*,
    // char*), confirmed by direct reading): if the object's own
    // process_input() apply exists, its *return value* decides what
    // actually reaches parse_command() -- a string return is the line to
    // dispatch instead of the original (real mud-shell/alias/history
    // preprocessing, confirmed live in this mudlib's own std/user/
    // nmsh.c), a truthy non-string return means the input was fully
    // consumed and nothing dispatches at all, and anything else
    // (function genuinely undefined, or returns a falsy number) falls
    // through to dispatching the original line unchanged -- matching
    // real comm.c's "if (!ret) ... parse_command(user_command, ...)" /
    // "if (ret->type == T_STRING) parse_command(buf, ...)" / "if
    // (ret->type != T_NUMBER || !ret->u.number) parse_command(user_command,
    // ...)" three-way branch exactly.
    auto obj = conn.boundObject();
    if (!obj) return;

    std::string toDispatch = line;
    Value processed = vm.callFunction(obj, "process_input", {Value(line)});
    if (std::holds_alternative<std::string>(processed.data)) {
        toDispatch = std::get<std::string>(processed.data);
    } else if (std::holds_alternative<int64_t>(processed.data)) {
        if (std::get<int64_t>(processed.data) != 0) return; // fully consumed
    }
    // monostate (process_input undefined) or any other falsy/non-string
    // return: dispatch the original line, untouched.

    // real parse_command(): matches against command_giver's own action
    // table (see VM::dispatchCommand()'s own comment for the exact
    // add_action/enable_commands semantics this backs).
    bool claimed = vm.dispatchCommand(obj, toDispatch);

    // real notify_no_command() (add_action.c): fires only when the
    // whole action-table walk ended with nothing claiming the command --
    // dispatchCommand() returning false is exactly that condition (a
    // truthy handler return makes it return true immediately, and real
    // user_parser() skips notify_no_command() on that same path). A
    // string is shown directly; a function is called with no arguments
    // and, if *it* returns a string, that string is shown instead -- a
    // non-string return from the function shows nothing at all, matching
    // real semantics exactly (not "show the function itself" or "show
    // nothing whenever a function was set"). If notify_fail() was never
    // called during this dispatch, nothing further happens here,
    // deliberately: real notify_no_command()'s own hardcoded "What?\n"
    // default is left as a mudlib-level concern, matching this driver's
    // pre-existing scoping decision for the "no action matched at all"
    // case (std/living.c's own cmd_hook() already sends its own default
    // via "if(query_client()) receive(\"<error>\");" plus its own SOUL_D/
    // CHAT_D fallback chain) -- notify_fail() extends that same decision
    // rather than overriding it: this driver shows a message *this
    // mudlib's own code explicitly asked to be shown*, never one this
    // driver invents on its own.
    if (!claimed) {
        if (auto pending = conn.takePendingNotifyFail()) {
            if (auto* msg = std::get_if<std::string>(&pending->data)) {
                deliverToConnection(vm, &conn, *msg);
            } else if (auto* closure = std::get_if<std::shared_ptr<Closure>>(&pending->data)) {
                if (*closure) {
                    Value result = vm.callClosure(*closure, {});
                    if (auto* resultMsg = std::get_if<std::string>(&result.data)) {
                        deliverToConnection(vm, &conn, *resultMsg);
                    }
                }
            }
        }
    }
}

void Server::fireNetDeadIfLinkDead(VM& vm, Connection& conn) {
    if (!conn.closed()) return;
    auto obj = conn.boundObject();
    if (!obj) return;

    OutputContext::set(&conn);
    try {
        vm.callFunction(obj, "net_dead", {});
    } catch (const std::exception& e) {
        std::cerr << "[net] connection fd=" << conn.fd()
                   << " net_dead() failed: " << e.what() << "\n";
    }
    OutputContext::set(nullptr);
}

void Server::pollSockets(VM& vm) {
    for (auto& sock : SocketRegistry::all()) {
        if (sock->fd < 0) continue;

        if (sock->mode == SocketMode::Datagram) {
            // Real read_socket_handler's own STATE_BOUND/DATAGRAM branch:
            // datagram sockets are read-polled regardless of state (no
            // DATA_XFER/connected concept for UDP) -- confirmed directly
            // against socket_read_select_handler()'s own switch, which
            // handles DATAGRAM identically whether STATE_BOUND or
            // (unreachable for datagram) otherwise.
            pollfd pfd{sock->fd, POLLIN, 0};
            if (::poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) continue;
            char buf[2048];
            sockaddr_in peer{};
            socklen_t peerLen = sizeof(peer);
            ssize_t n = ::recvfrom(sock->fd, buf, sizeof(buf) - 1, 0,
                                    reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string addr = std::string(::inet_ntoa(peer.sin_addr)) + " " +
                                std::to_string(ntohs(peer.sin_port));
            // Real: push_number(fd); push string; push addr;
            // call_callback(fd, S_READ_FP, 3) -- three args.
            fireSocketCallback(vm, sock->readCallback, sock->owner,
                {Value(static_cast<int64_t>(sock->handle)), Value(std::string(buf)), Value(addr)});
            continue;
        }

        if (sock->state == SocketState::Listen) {
            // Real S_WACCEPT: only re-arm/re-fire once socket_accept()
            // actually consumes the pending connection.
            if (sock->acceptPending) continue;
            pollfd pfd{sock->fd, POLLIN, 0};
            if (::poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) continue;
            sock->acceptPending = true;
            // Real: push_number(fd); call_callback(fd, S_READ_FP, 1) --
            // one arg, the listening socket's own handle, not a new one
            // (socket_accept() itself, called from LPC in response, is
            // what actually performs accept() and returns the new
            // handle -- see SocketRegistry::accept()).
            fireSocketCallback(vm, sock->readCallback, sock->owner,
                {Value(static_cast<int64_t>(sock->handle))});
            continue;
        }

        if (sock->state != SocketState::DataXfer) continue;

        short events = POLLIN;
        if (sock->blocked) events |= POLLOUT;
        pollfd pfd{sock->fd, events, 0};
        if (::poll(&pfd, 1, 0) <= 0) continue;

        if (pfd.revents & POLLIN) {
            char buf[4096];
            ssize_t n = ::read(sock->fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                // Real: push_number(fd); push string; call_callback(fd,
                // S_READ_FP, 2) -- two args for STREAM (MUD mode's own
                // three-arg svalue form is not implemented, see
                // LpcSocket.hpp's own SocketMode comment).
                fireSocketCallback(vm, sock->readCallback, sock->owner,
                    {Value(static_cast<int64_t>(sock->handle)), Value(std::string(buf))});
            } else if (n == 0) {
                // Peer closed cleanly. Real socket_read_select_handler()
                // itself does nothing special on cc<=0 (just breaks out
                // of its switch); the actual close/close_callback firing
                // for a dead peer happens elsewhere in real comm.c's own
                // outer select loop, outside socket_efuns.c's scope this
                // driver has vendored source for. Closing here on a
                // clean EOF (rather than leaving the socket to spin
                // forever re-detecting the same readable-with-nothing-
                // to-read state) is a deliberate, pragmatic choice for
                // this driver, not a literal port of unread reference
                // code.
                closeSocketAndFireCallback(vm, sock);
                continue;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                closeSocketAndFireCallback(vm, sock);
                continue;
            }
        }

        if ((pfd.revents & POLLOUT) && sock->blocked) {
            if (!sock->pendingWrite.empty()) {
                ssize_t off = ::write(sock->fd, sock->pendingWrite.data(), sock->pendingWrite.size());
                if (off > 0) {
                    sock->pendingWrite.erase(0, static_cast<size_t>(off));
                } else if (off < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    closeSocketAndFireCallback(vm, sock);
                    continue;
                }
            }
            if (sock->pendingWrite.empty()) {
                // Real socket_write_select_handler(): "flags &=
                // ~S_BLOCKED; ... call_callback(fd, S_WRITE_FP, 1);" --
                // fires identically whether this was a flushed partial
                // write or a completed non-blocking connect() (both set
                // S_BLOCKED the same way; see SocketRegistry::connect()'s
                // own comment).
                sock->blocked = false;
                fireSocketCallback(vm, sock->writeCallback, sock->owner,
                    {Value(static_cast<int64_t>(sock->handle))});
            }
        }
    }
}

void Server::handleConnection(Connection& conn) {
    auto lines = conn.pollLines();

    auto obj = conn.boundObject();

    // Real comm.c: "apply(APPLY_WINDOW_SIZE, ip->ob, 2, ORIGIN_DRIVER)"
    // fires on whichever object is currently bound the moment a NAWS
    // subnegotiation is parsed -- independent of whether any text line
    // was also completed in this same poll (NAWS data is out-of-band
    // control data, not line data).
    if (conn.takeWindowSizeUpdate() && obj) {
        OutputContext::set(&conn);
        try {
            vm_.callFunction(obj, "window_size",
                {Value(static_cast<int64_t>(conn.terminalWidth())),
                 Value(static_cast<int64_t>(conn.terminalHeight()))});
        } catch (const std::exception& e) {
            std::cerr << "[net] connection fd=" << conn.fd()
                       << " window_size() failed: " << e.what() << "\n";
        }
        OutputContext::set(nullptr);
    }

    // Real comm.c: "apply(APPLY_TERMINAL_TYPE, ip->ob, 1, ORIGIN_DRIVER)"
    // fires on whichever object is currently bound the moment an SB TTYPE
    // IS response is parsed -- same shape as the window_size block just
    // above, independent of whether any text line was also completed in
    // this same poll.
    if (conn.takeTerminalTypeUpdate() && obj) {
        OutputContext::set(&conn);
        try {
            vm_.callFunction(obj, "terminal_type", {Value(conn.terminalType())});
        } catch (const std::exception& e) {
            std::cerr << "[net] connection fd=" << conn.fd()
                       << " terminal_type() failed: " << e.what() << "\n";
        }
        OutputContext::set(nullptr);
    }

    if (obj && !lines.empty()) {
        OutputContext::set(&conn);
        for (const auto& line : lines) {
            // Real current FluffOS's own per-command recovery boundary
            // (packages/core/add_action.cc's safe_parse_command(), and
            // comm.cc's own safe_apply(APPLY_PROCESS_INPUT, ...) call
            // immediately above it in process_input()): every risky call
            // in the real per-line dispatch path is individually wrapped
            // in a fresh error_context_t (save_context()/try/catch(const
            // char*)/restore_context()/pop_context()), so an uncaught
            // error aborts only the one command running inside it -- the
            // player sees an error message (real _error_handler(),
            // simulate.cc: full trace for a wizard, DEFAULT_ERROR_MESSAGE
            // otherwise) and the connection, and the rest of that
            // player's already-buffered input, both continue normally.
            // (Real FluffOS 2.9 ds2.08 achieved the same outcome, but
            // only as a side effect of one single top-level SETJMP set up
            // once in backend() before its own command loop -- current
            // FluffOS's per-call safe_apply()/safe_parse_command() wrapping
            // is the more explicit, load-bearing version of the same
            // guarantee, not a behavior change a player would observe.)
            //
            // This driver's own dispatchLine() is the equivalent of that
            // entire process_input()/parse_command() chain collapsed into
            // one call, so one try/catch around it here is the matching
            // boundary. Unlike real FluffOS's SETJMP (which cannot run
            // C++ destructors and needs restore_context() to manually
            // reset sp/csp/cgsp by hand), this driver's own VM state
            // (callStack_, objectChangeStack_, originStack_, commandGiver
            // stack, and run()'s own per-call locals/operand stack) is
            // already exception-safe by construction -- every one of
            // those is popped by an RAII guard's destructor (see
            // ObjectFrameGuard/CommandGiverGuard/OriginGuard in VM.cpp,
            // and run()'s own "RAII rather than an explicit pop" comment
            // on objectFrameGuard), so ordinary C++ stack unwinding
            // already does restore_context()'s whole job for free, with
            // no separate reset step needed here at all. evalCost_ needs
            // no reset either: it is a plain counter, not a stack, and
            // dispatchLine() already unconditionally resets it (real
            // process_user_command()'s own "eval_cost reset to 0 once at
            // the start of each top-level dispatch") the moment the next
            // line runs, whether or not this one threw.
            try {
                dispatchLine(vm_, conn, line);
            } catch (const std::exception& e) {
                std::cerr << "[net] connection fd=" << conn.fd()
                           << " input handling failed (command isolated, "
                              "connection stays open): " << e.what() << "\n";
                // Tell the player something happened -- matching real
                // _error_handler()'s own guarantee that command_giver
                // always sees *some* message, not silence. This driver has
                // no wizard/mortal distinction to gate on (unlike real
                // FluffOS's O_IS_WIZARD check), so every player gets the
                // same generic message; the real exception detail goes to
                // the driver's own log above instead, not to the player,
                // matching real DEFAULT_ERROR_MESSAGE's own intent of not
                // leaking internal detail to an ordinary player. Wrapped in
                // its own try/catch: deliverToConnection() can itself
                // throw (a snoop relay's own receive_snoop() erroring), and
                // an error while reporting an error must not undo this
                // fix by escaping uncaught back into the loop below.
                try {
                    deliverToConnection(vm_, &conn,
                        "Error while processing your command.\n");
                } catch (const std::exception& e2) {
                    std::cerr << "[net] connection fd=" << conn.fd()
                               << " error-report delivery itself failed: "
                               << e2.what() << "\n";
                }
                // No markClosed(): this player's connection, and the rest
                // of their already-buffered input this same poll, both
                // continue -- only the one command that threw is aborted.
                continue;
            }
        }
        OutputContext::set(nullptr);
    }

    // Catches the case pollLines() just detected: the peer's socket is
    // gone (EOF/read error), but close() itself (and the InteractiveRegistry
    // removal/fd close it does) hasn't run yet -- see fireNetDeadIfLinkDead's
    // own comment for why this is also correctly a no-op for every other
    // way a connection ends up closed.
    fireNetDeadIfLinkDead(vm_, conn);
}

void Server::pollOnce() {
    if (listenFd_ < 0) return;

    acceptNewConnections();

    for (auto& conn : connections_) {
        if (conn->isOpen()) {
            handleConnection(*conn);
        }
    }

    pollSockets(vm_);

    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                        [](const std::shared_ptr<Connection>& c) { return c->closed(); }),
        connections_.end());
}

} // namespace amlp
