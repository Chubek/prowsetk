# Native Flatworm JavaScript module

`math.c` uses only the standalone C ABI in `include/Flatwork-Module.h`. It
exports `add(a, b)` and `pi` to page JavaScript. `embed.cpp` loads the library
into the browser's module registry before creating a session, then runs an
offline module script that writes `42` into the DOM.

```sh
cmake --preset default
cmake --build --preset default
build/default/examples/prowsetk_example_flatworm_modules \
  ./build/default/examples/libflatworm_math.so
```

An independently built library needs no link to ProwseTk or QuickJS:

```sh
cc -std=c11 -fPIC -shared -Iinclude examples/flatworm-module/math.c \
  -o build/flatworm_math.so
```

Classic scripts use `Flatworm.module('math').add(20, 22)`. ECMAScript module
scripts use `import {add, pi} from 'flatworm:math'`. The embedder selects the
native libraries; page scripts can only resolve already-installed modules.
The runtime owns each module instance and keeps native code loaded until
teardown. See [Manual Chapter 12](../../manual/12-javascript.md) for lifecycle,
typed arguments/results, resource bounds, and the C++ loading interfaces.
