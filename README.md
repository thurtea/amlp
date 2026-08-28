# amlp

A custom, from-scratch C++20 LPC game driver: lexer, parser, bytecode VM, and a TCP server for running an LPC mudlib. Supports several LPC language dialects behind a single `dialect` config switch. See `CREDITS.md` for what shaped this driver's design, and `docs/COMPARISON.md` for a detailed, evidence-based feature comparison against other LPC drivers.

## Build and run

```
cmake -B build -S .
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

```
./build/amlp etc/driver.cfg
```

boots the driver's own bundled mudlib under `mudlib/`.
