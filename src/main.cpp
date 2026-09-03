#include <iostream>
#include <cstdlib>
#include <csignal>
#include "amlp/config/Config.hpp"
#include "amlp/object/ObjectManager.hpp"
#include "amlp/vm/VM.hpp"
#include "amlp/net/Server.hpp"
#include "amlp/scheduler/Scheduler.hpp"
#include "amlp/efun/EfunTable.hpp"
#include "amlp/dialect/DialectSelect.hpp"
#include "amlp/dialect/MasterUidBoot.hpp"
#include "amlp/dialect/InaugurateMasterBoot.hpp"

namespace {
void handleSignal(int) {
    amlp::Scheduler::requestShutdown();
}
}

// real production servers all do this; this driver never did. A client
// connection closing at the wrong moment relative to this process's own
// next write()/send() to that same socket raises SIGPIPE, whose default
// disposition terminates the *entire process* -- confirmed live,
// reproduced once as a background driver instance exiting with code 141
// (the standard 128+SIGPIPE shell convention) right after a test
// client's socket closed, see STATUS.md's own dated entry and
// src/net/instruct.md's own "Known gap" note, both now closed by this
// fix. Ignoring the signal here is the standard, low-risk fix: every
// write()/send() that would have raised it instead just returns -1 with
// errno set to EPIPE, a completely ordinary failed-write return value
// this codebase's own connection-handling code already has to tolerate
// regardless (a peer that vanished between the read that detected EOF
// and this process's own next write to it is not a new failure mode --
// see Connection.cpp/Server.cpp's own existing handling for a closed
// connection). No interaction with the existing SIGINT/SIGTERM handling
// below: SIGPIPE is a distinct signal number, ignoring it changes
// nothing about how those two are delivered or handled.

int main(int argc, char** argv) {
    // No fallback config path: this used to default to "config/driver.cfg",
    // a path that has never existed anywhere in this repo (found during a
    // dependency grep, not acted on until now) -- a silently-dead branch,
    // since every real invocation of this binary, throughout this
    // project's own history (README.md, every live-verification session
    // recorded in STATUS.md), already passes an explicit config path.
    // Repointing the default at etc/driver.cfg (now the one canonical
    // config, see STATUS.md's consolidation entry) was considered instead
    // of removing it, but would still silently assume the process runs
    // from the repo root -- an assumption nothing else here makes, and
    // one real invocation pattern has never actually relied on. Failing
    // loudly with a usage message is more honest than guessing a path.
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config-path> [max-iterations]\n";
        return 1;
    }
    std::string configPath = argv[1];

    int maxIterations = 0;
    if (argc > 2) {
        maxIterations = std::atoi(argv[2]);
    } else if (const char* envVal = std::getenv("AMLP_MAX_ITERATIONS")) {
        maxIterations = std::atoi(envVal);
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    amlp::Config config;
    if (!config.loadFromFile(configPath)) {
        std::cerr << "Failed to load config: " << configPath << "\n";
        return 1;
    }

    amlp::registerCoreEfuns();

    amlp::ObjectManager objectManager(config);
    amlp::VM vm(objectManager, config);
    objectManager.setVM(&vm);
    // See ObjectManager::setEfunExistsChecker()'s own comment: this is
    // the one place that can safely see both EfunTable (already
    // populated by registerCoreEfuns() above) and ObjectManager without
    // creating a link cycle between the object and efun libraries.
    objectManager.setEfunExistsChecker([](const std::string& name) {
        return amlp::EfunTable::instance().exists(name);
    });

    amlp::Scheduler scheduler(vm);
    vm.setScheduler(&scheduler);
    amlp::Server server(config, vm, objectManager, scheduler);

    std::cout << "amlp booting...\n";
    std::cout << "  mudlib_root = " << config.mudlibRoot() << "\n";
    std::cout << "  master_file = " << config.masterFile() << "\n";
    std::cout << "  port        = " << config.port() << "\n";

    // Real FluffOS's own actual boot order, confirmed directly against
    // the vendored 2.9 reference's own real main.c (not assumed):
    // "save_context(&econ); if (SETJMP(econ.context)) { debug_message(
    // \"The simul_efun (%s) and master (%s) objects must be loadable.
    // \n\", SIMUL_EFUN, MASTER_FILE); exit(-1); } else { init_simul_efun
    // (SIMUL_EFUN); init_master(); }" (main.c:311-319) -- simul_efun
    // loads *first*, master second, the reverse of this driver's own
    // prior order. Real LDMud takes a different, lazy approach instead
    // (assert_simul_efun_object(), src/simul_efun.c, called on-demand
    // from interpret.c at an actual simul_efun call site, not eagerly at
    // boot at all -- the master-then-inaugurate_master ordering the
    // comment on applyInaugurateMaster() below cites, real LDMud
    // src/main.c:661-687, is about LDMud's own master/inaugurate_master
    // sequence specifically, not a claim that simul_efun must load after
    // master there too), but loading simul_efun eagerly, before master,
    // is still fully compatible with that lazy real semantics -- nothing
    // about "load it lazily on first use" is violated by a driver that
    // simply chooses to have it already available whenever that first
    // use happens to come. Found live necessary for FluffOS specifically
    // against a real third-party mudlib corpus (Dead Souls 3.8.2's own
    // boot attempt): secure/daemon/master.c's own real create() calls
    // file_exists() (a real, genuine simul_efun, secure/sefun/files.c),
    // and this driver's own prior master-first order meant
    // objects_.simulEfunObject() was always still null at that point,
    // for every single mudlib, not just this one -- "undefined function
    // or efun: file_exists" every time, regardless of dialect.
    if (!config.simulEfunFile().empty()) {
        if (objectManager.loadSimulEfunObject()) {
            std::cout << "Simul_efun object loaded: " << config.simulEfunFile() << "\n";
        } else {
            std::cerr << "Warning: failed to load simul_efun object "
                       << config.simulEfunFile() << " (continuing without it)\n";
        }
    }

    if (!objectManager.loadMasterObject()) {
        std::cerr << "Failed to load master object: " << config.masterFile() << "\n";
        return 1;
    }

    std::cout << "Driver booted. Master object loaded: " << config.masterFile() << "\n";

    // Real per-dialect boot-time master UID query (see
    // src/dialect/MasterUidBoot.hpp). BootApi is now genuinely
    // config-driven -- Config::dialect() defaults to "fluffos", matching
    // this driver's own prior hardcoded behavior exactly for any config
    // file that never sets the key. "dgd" throws (DgdBootApi does not
    // exist yet, out of scope) -- see DialectSelect.hpp.
    auto bootApi = amlp::makeBootApiForConfig(config);
    if (auto uid = amlp::queryMasterUid(vm, *bootApi)) {
        std::cout << "  master " << bootApi->masterUidApply() << "() = \"" << *uid << "\"\n";
    } else {
        std::cout << "  master does not define " << bootApi->masterUidApply() << "()\n";
    }

    // Real LDMud's own inaugurate_master(0) boot callback (ROADMAP.md
    // row 1.7/1.8; see BootApi::inaugurateMasterApply()'s own comment
    // and InaugurateMasterBoot.cpp for the full real-source citation) --
    // the real trigger for a real LDMud master's own addDriverHooks(),
    // and the actual reason set_driver_hook()/H_MOVE_OBJECT0 dispatch
    // (built over the last two sessions) is reachable at all from a real
    // mud simply coming online, not just by hand. Placed here, right
    // after the master UID query, matching real LDMud's own exact real
    // ordering (main.c:661-663 runs before assert_simul_efun_object() at
    // main.c:687 -- LDMud's own master-then-simul_efun relative order,
    // unaffected by moving *this driver's own* simul_efun load earlier;
    // see the real FluffOS citation above this function's own simul_efun
    // load for why that move was needed). A no-op under FluffOS/DGD (no
    // equivalent apply exists there at all).
    if (auto inaugurateApply = bootApi->inaugurateMasterApply()) {
        std::cout << "  master " << *inaugurateApply << "(0) ...\n";
    }
    amlp::applyInaugurateMaster(vm, *bootApi);

    if (!server.listen()) {
        std::cerr << "Failed to start network listener on port " << config.port() << "\n";
        return 1;
    }

    std::cout << "Ready for connections. Try: telnet localhost " << config.port() << "\n";
    if (maxIterations > 0) {
        std::cout << "(test mode: will exit after " << maxIterations << " poll iterations)\n";
    } else {
        std::cout << "(press Ctrl-C to stop)\n";
    }

    scheduler.run(server, maxIterations);

    std::cout << "amlp shutting down.\n";
    return 0;
}
