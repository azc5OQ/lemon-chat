# Supplying memory manager functions to meek-ui

meek-ui allocates through its memory manager (`src/clib/memory_manager.c`).
The same `memory_manager.c` can be compiled into more than one module, for
example into a program and into the module that links meek-ui. The copy
that is initialised first owns the memory. It passes the addresses of its
own four functions to the copy initialised later:

```c
&memory_manager__alloc
&memory_manager__calloc
&memory_manager__realloc
&memory_manager__free
```

The later copy then forwards every allocation, reallocation and release to
those functions and stores no allocation data of its own. The process has
one memory manager: the one initialised first.

This document lists the exact types, the calls and their order.

## 1. The structure

The memory manager's own interface is `memory_manager_functions_t` in
`src/clib/memory_manager.h` (the four function slots and `owner`),
installed with `memory_manager__delegate_to` before `memory_manager__init`.
A meek-ui host supplies the same four functions in a `meek_host_services_t`,
declared in `src/host_services.h`, together with the log and file slots,
and installs it with:

```c
void host_services__set(const meek_host_services_t* services);
```

`host_services__set` hands the heap slots and `owner` to
`memory_manager__delegate_to`. The rest of this document uses the
`meek_host_services_t` form; a project that uses the memory manager without
meek-ui fills `memory_manager_functions_t` directly, with the same field
names.

`host_services__set` copies the structure, so it can be a local variable.
The functions it points to must remain valid until the later copy's
`memory_manager__shutdown` has returned.

The allocation fields. Each field type is the type of the memory manager
function in the last column, so that function's address is assigned without
a cast:

| Field | Type | Signature of the function | Required | Memory manager function |
|---|---|---|---|---|
| `allocate` | `meek_allocate_fn` | `void* f(nuint size, uint type)` | yes | `memory_manager__alloc` |
| `allocate_zeroed` | `meek_allocate_zeroed_fn` | `void* f(nuint count, nuint size, uint type)` | no | `memory_manager__calloc` |
| `reallocate` | `meek_reallocate_fn` | `void* f(void* ptr, nuint new_size)` | yes | `memory_manager__realloc` |
| `release` | `meek_release_fn` | `void f(void* ptr)` | yes | `memory_manager__free` |
| `owner` | `const void*` | not a function | no | set by `memory_manager__export_services`; NULL otherwise |

`allocate`, `reallocate` and `release` are used only when all three are
set. If one of them is NULL, the later copy ignores all three.

`nuint` is an unsigned integer with the width of a pointer (`src/types.h`):
32 bits in an x86 build, 64 bits in an x64 build. It has the same width as
`size_t`. `uint` is `unsigned int`.

`type` is the type number the caller allocated the block with. The later
copy passes it on unchanged, so the copy initialised first records each
block under the number the later module chose (section 4.1).

The structure has three further fields that are not related to memory:
`write_log`, `read_file` and `free_file`. They are described in the
[embedded README](README.md#what-the-static-library-needs-from-its-host).
Zero the whole structure before filling it, so that fields you do not set
are NULL.

## 2. Requirements for the functions

`memory_manager__alloc`, `memory_manager__calloc`, `memory_manager__realloc`
and `memory_manager__free` meet these requirements. A different memory
manager (section 4.2) has to meet them as well. Each function must:

- have exactly the signature in the table;
- use the compiler's default calling convention, which is `__cdecl` on
  32-bit x86. A `__stdcall` function, for example a Win32 function such as
  `HeapAlloc`, needs an adapter function with the default convention. A
  cast to the field's type compiles for a `__stdcall` function but does not
  change its convention, and the first call then corrupts the stack;
- return memory aligned for any fundamental type (pointer, `int64`,
  `double`), as `malloc` does. The later copy places no header in front of
  the block, so the caller receives that alignment unchanged;
- for `reallocate`: keep the block's contents up to the smaller of the old
  and new size, and on failure return NULL and leave the original block
  valid and unchanged;
- be safe to call from every thread that calls into meek-ui. The later
  copy takes no lock of its own.

The later copy does not make the following calls, so the functions do not
need to handle them:

| Call | What the later copy does instead |
|---|---|
| `allocate(0, type)` | returns NULL without calling the function |
| `allocate_zeroed` with a zero count or size | returns NULL without calling the function |
| `allocate_zeroed` where `count * size` overflows | returns NULL without calling the function |
| `reallocate(NULL, size)` | calls `allocate(size, 0)` |
| `reallocate(ptr, 0)` | calls `release(ptr)` and returns NULL |
| `release(NULL)` | returns without calling the function |

If `allocate_zeroed` is not set, the later copy calls `allocate` and writes
zeros over the block itself.

The later copy calls `reallocate` also when the size does not change,
because it does not store block sizes. `memory_manager__realloc` returns
the block unchanged for an unchanged size. With the C runtime's realloc
behind it instead, a same-size call measured about 100 ns
(`docs/stdlib-and-memory.md`).

## 3. Order of calls

Startup:

```c
/* 1. the copy initialised first starts (module A) */
memory_manager__init();

/* 2. the later copy receives module A's functions (section 4) */
host_services__set(&services);

/* 3. the later copy starts; from here it forwards to module A */
memory_manager__init();

/* 4. the rest of meek-ui's startup, as in README.md "Host sequence" */
widget_registry__bootstrap_builtins();
embedded_d3d9__attach(device);
font__init();
/* ... */
```

Steps 2 to 4 run in the module that links meek-ui (module B). Step 1 runs in
module A.

Shutdown:

```c
/* 1. meek-ui releases what it holds (module B) */
hot_reload__shutdown();
font__shutdown();
embedded_d3d9__detach();

/* 2. the later copy stops (module B) */
memory_manager__shutdown();

/* 3. the copy initialised first may stop only after this point (module A) */
memory_manager__shutdown();
```

The later copy decides at its `memory_manager__init` whether it forwards.
If it allocates before `memory_manager__init`, it decides at that first
allocation. The decision stays in force until its `memory_manager__shutdown`.
Consequences:

- `host_services__set` has to run before `memory_manager__init`. Functions
  supplied later are not used until the copy is shut down and initialised
  again.
- The four heap functions are copied at that moment. Calling
  `host_services__set` while the copy is initialised changes the log and
  file slots only; the blocks stay with the heap that allocated them. To
  replace the heap functions, call `memory_manager__shutdown`, then
  `host_services__set`, then `memory_manager__init`.
- A copy that still holds blocks of its own (allocated standalone before
  init, or left live at an earlier shutdown) does not delegate: it logs
  `delegation refused` and stays standalone, because those blocks would
  otherwise be released through the other heap. Release them, shut down
  and initialise again.

## 4. Supplying the functions

### 4.1 Another copy of meek-ui's memory manager (module A and module B)

Module A contains `memory_manager.c` and is initialised first. Module B
links meek-ui, which contains the second copy. Module A passes the
addresses of its four functions to module B, for example as the parameters
of an initialisation function that module B exports:

```c
/* module B: declared in a header that module A includes */
int overlay__init(IDirect3DDevice9* device, void* (*allocate)(nuint size, uint type), void* (*allocate_zeroed)(nuint count, nuint size, uint type), void* (*reallocate)(void* ptr, nuint new_size), void (*release)(void* ptr));
```

```c
/* module A, after its own memory_manager__init */
overlay__init(device, &memory_manager__alloc, &memory_manager__calloc, &memory_manager__realloc, &memory_manager__free);
```

```c
/* module B: the body of overlay__init */
meek_host_services_t services;

stdlib__null_memory(&services, sizeof(services));
services.allocate        = allocate;
services.allocate_zeroed = allocate_zeroed;
services.reallocate      = reallocate;
services.release         = release;
host_services__set(&services);
memory_manager__init();
/* ... the rest of meek-ui's startup ... */
```

The parameter types are the functions' own types, so the compiler rejects a
function with another signature. No cast is needed at either end.

`memory_manager__export_services(&services)` fills the same four fields
with the same four addresses and also sets `owner`. Module A can use it
instead of assigning the fields, when it passes the whole structure.

Both modules must be built from the same meek-ui version, so that
`meek_host_services_t` and the function types are the same in both.

Result:

- Module B's blocks are ordinary blocks of module A's memory manager. They
  appear in module A's statistics and leak report.
- Each block is counted under the type number module B allocated it with.
  meek-ui's own numbers are in `src/memory_types.h` (`MM_TYPE__GENERIC = 0`
  to `MM_TYPE__FS = 7`). The memory manager stores numbers and does not
  translate them to names. If module A uses its own numbers, the two
  codebases choose numbers that do not overlap.
  `memory_manager_stats_t.live_bytes_by_type` has
  `MEMORY_MANAGER__TYPE_COUNTERS` (256) counters; numbers from 255 upwards
  share the last one.
- A block allocated by one module can be released by the other.

A memory manager does not forward to its own functions. If the structure
holds the functions of the same copy, recognised by `owner` or by the
function addresses, `memory_manager__init` prints `delegation refused` and
that copy allocates from the C runtime instead. In a build with
`MEMORY_MANAGER__NO_COMMON_RUNTIME` there is no C runtime heap (and nothing
is printed), so every allocation then returns NULL unless
`memory_manager__init_win32` supplied a heap. Forwarding to itself would make each
allocation call itself without end. In 4.1 module A's addresses differ from
module B's, so this does not apply.

### 4.2 A different memory manager

A memory manager that is not `memory_manager.c` supplies functions with
the signatures from the table in section 1. If its functions have other
signatures, write adapter functions. Example for a manager whose allocate
has no type parameter and whose release has a second parameter:

```c
void* my_memory__alloc(nuint size);
void  my_memory__free(void* ptr, boole clear_memory);

static void* my_memory_allocate(nuint size, uint type)
{
    (void)type;
    return my_memory__alloc(size);
}

static void my_memory_release(void* ptr)
{
    my_memory__free(ptr, FALSE);
}

services.allocate = my_memory_allocate;
services.release  = my_memory_release;
```

Assigning `my_memory__free` directly is an incompatible pointer assignment,
which the compiler reports as a warning or an error depending on the
compiler and its settings. Casting it to `meek_release_fn` removes the
diagnostic, and meek-ui would then call it with an undefined second
argument.

If this manager is linked into the same module as meek-ui, its function
names must differ from meek-ui's (`memory_manager__init`,
`memory_manager__shutdown`, `memory_manager__alloc`,
`memory_manager__calloc`, `memory_manager__realloc`,
`memory_manager__free`), or the linker reports duplicate symbols.

### 4.3 Addresses received without a type

When module B receives the addresses as `void*` or as `FARPROC` from
`GetProcAddress`, it casts them to the field types:

```c
services.allocate   = (meek_allocate_fn)(void (*)(void))GetProcAddress(module_a, "memory_manager__alloc");
services.reallocate = (meek_reallocate_fn)(void (*)(void))GetProcAddress(module_a, "memory_manager__realloc");
services.release    = (meek_release_fn)(void (*)(void))GetProcAddress(module_a, "memory_manager__free");
```

The cast does not check anything. The functions at those addresses must
have the signatures and the calling convention from sections 1 and 2.
`tests/delegation_check.c` uses this form to call into `meek_ui.dll`.

### 4.4 The copy initialised first, without a C runtime (Windows)

On Windows the copy that owns memory (module A) allocates in standalone
mode through a table of Win32 functions, `memory_manager_win32_t`
(`memory_manager.h`; the memory manager stays two files, `memory_manager.h`
and `memory_manager.c`): `HeapAlloc`, `HeapReAlloc` and `HeapFree` on a heap
handle, and `InitializeCriticalSection`, `EnterCriticalSection`,
`LeaveCriticalSection` and `DeleteCriticalSection` for its mutex. In a
build with a C runtime, `memory_manager__init` fills the table itself with
the linked kernel32 functions and `GetProcessHeap()`. A program that links
no C runtime and resolves the Win32 functions itself defines
`MEMORY_MANAGER__NO_COMMON_RUNTIME` (presence only; its value is not read)
when compiling `memory_manager.c`: that removes the default fill, so
`memory_manager.c` references no C runtime or kernel32 function by name,
and module A passes the resolved addresses once instead:

```c
#include "clib/memory_manager.h"

memory_manager_win32_t win32;

stdlib__null_memory(&win32, sizeof(win32));
win32.heap_handle               = NULL;                          /* or a heap of your own */
win32.HeapCreate                = resolved.HeapCreate;
win32.HeapAlloc                 = resolved.HeapAlloc;
win32.HeapReAlloc               = resolved.HeapReAlloc;
win32.HeapFree                  = resolved.HeapFree;
win32.HeapDestroy               = resolved.HeapDestroy;
win32.InitializeCriticalSection = resolved.InitializeCriticalSection;
win32.EnterCriticalSection      = resolved.EnterCriticalSection;
win32.LeaveCriticalSection      = resolved.LeaveCriticalSection;
win32.DeleteCriticalSection     = resolved.DeleteCriticalSection;
if (memory_manager__init_win32(&win32) == FALSE)
{
    /* a required function is missing, HeapCreate failed, or the manager was initialised already */
}
```

The field types are the Win32 prototypes with the `WINAPI` convention,
written with the underlying types so that `memory_manager.h` does not
include `<windows.h>`; a resolved address cast to the field's type, or the
function itself, fits without an adapter.

| Field | Required | Use |
|---|---|---|
| `heap_handle` | no | the heap the blocks come from. NULL: the manager calls `HeapCreate(0, 0, 0)` at init and `HeapDestroy` at shutdown. Supplied: used as it is and never destroyed by the manager. A heap without `HEAP_GENERATE_EXCEPTIONS`, so that a failed allocation returns NULL |
| `HeapCreate`, `HeapDestroy` | when `heap_handle` is NULL | creating and destroying the manager's own heap |
| `HeapAlloc` | yes | `alloc`; `calloc` passes `HEAP_ZERO_MEMORY` |
| `HeapReAlloc` | yes | `realloc`; never called with a NULL block or a zero size |
| `HeapFree` | yes | `free` |
| `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection` | all three, or none | the manager's mutex. None: every run is single-threaded, as `memory_manager__init_single_threaded`, whichever init is called |
| `DeleteCriticalSection` | no | deleting the section at shutdown; without it the section is left as it is |

`memory_manager__init_win32` replaces `memory_manager__init` for that
copy: it copies the struct, creates the heap when none was supplied, and
initialises the manager. It returns FALSE and initialises nothing when a
required function is missing, when `HeapCreate` fails, or when the manager
is initialised already. The struct is kept across `memory_manager__shutdown`,
so a plain `memory_manager__init` afterwards reuses the functions and
creates the heap again when it was created by the manager. A shutdown that
destroys the manager's own heap releases every block on it, leaked ones
included, after the leak report.

With the switch defined, nothing can be allocated before
`memory_manager__init_win32`: a standalone `memory_manager__init` without
supplied functions initialises nothing (and prints nothing, there is no
C runtime), and every allocation returns NULL.

Module B (the copy initialised later, section 4.1) is compiled with the
same switch and needs no Win32 function: it calls `memory_manager__init`
after `host_services__set` as before and delegates. The order of calls in
section 3 is unchanged.

On platforms other than Windows the switch has no effect: the standalone
heap stays `malloc`, `realloc` and `free`.

Checked by `tests/memory_manager_check.c`, part 3, built as
`memory_manager_win32_check` (GCC) and by `tests/freestanding/check.py
--suite memory` (clang-cl, x86 and x64); `check.py` with the default
freestanding suite links the whole library with the switch and
`/nodefaultlib` and confirms that no C runtime import appears.

## 5. What the memory manager does in each mode

| | Functions supplied (delegating) | No functions supplied (standalone) |
|---|---|---|
| Memory comes from | the supplied functions | the C runtime (Windows: the Win32 functions in the manager's table, section 4.4); with `MEMORY_MANAGER__NO_COMMON_RUNTIME` only what `memory_manager__init_win32` supplied, otherwise nothing |
| Pointer returned by `GUI_MALLOC` | the pointer the supplied `allocate` returned | a pointer behind a header of this copy |
| Block list, statistics, leak report | none in this copy; the supplied manager keeps them | kept by this copy |
| Lock | none in this copy | this copy's own mutex, unless `memory_manager__init_single_threaded` |
| `memory_manager__get_stats()` | all fields zero | current figures |
| Type numbers | passed to `allocate` and `allocate_zeroed` unchanged | stored per block, counted in `live_bytes_by_type` |

## 6. Checking the result

After `memory_manager__init` in module B:

```c
if (memory_manager__is_delegating() == FALSE)
{
    /* the functions are not in use; see section 7 */
}
```

A block allocated through meek-ui is the block the supplied `allocate`
returned:

```c
void* probe = GUI_MALLOC(32);   /* allocate was just called with 32 and type 0 */
GUI_FREE(probe);                /* release receives the same pointer */
```

After module B's shutdown (section 3), module A's memory manager reports
the same number of live blocks as before meek-ui started.

## 7. Symptoms and causes

| Symptom | Cause | Change |
|---|---|---|
| `memory_manager__is_delegating()` returns FALSE after `memory_manager__init` | `host_services__set` ran after `memory_manager__init`, or after meek-ui's first allocation | call `host_services__set` first (section 3) |
| same | one of `allocate`, `reallocate`, `release` is NULL | set all three |
| same, and stdout shows `delegation refused ... this manager's own` | the structure holds the functions of the same copy of the memory manager | pass module A's functions to module B (4.1) |
| same, and stdout shows `delegation refused ... still live` | module B allocated standalone before `host_services__set`, or an earlier run of module B left blocks live | install the functions before the first allocation; release every block before `memory_manager__shutdown` |
| compiler reports an incompatible pointer type at the assignment | the function's signature differs from the field type | use the memory manager's functions, or an adapter (4.2) |
| linker reports duplicate `memory_manager__alloc` or similar | a different manager with the same function names is linked into meek-ui's module | rename its functions (4.2) |
| crash or stack corruption at the first allocation, 32-bit x86 build | a `__stdcall` function in a field that expects the default convention | call it through an adapter with the default convention |
| every allocation returns NULL and nothing is drawn | built with `MEMORY_MANAGER__NO_COMMON_RUNTIME` and neither another copy's functions nor `memory_manager__init_win32` supplied a heap | supply the functions before `memory_manager__init`, or call `memory_manager__init_win32` (4.4) |
| module A reports meek-ui blocks as live after shutdown | module B's shutdown calls (section 3) did not run | run the shutdown sequence |

## 8. Code that uses this

- `tests/delegation_check.c`: two copies of the memory manager, the test
  executable's and `meek_ui.dll`'s. The executable assigns
  `&memory_manager__alloc`, `&memory_manager__calloc`,
  `&memory_manager__realloc` and `&memory_manager__free` and installs them
  in the DLL's copy (4.1). It checks that the DLL's blocks are counted in
  the executable's copy under the DLL's type numbers, and that a block made
  on one side is released on the other.
- `tests/host_services_check.c`: the calls in the table of section 2, the
  type number reaching `allocate` and `allocate_zeroed` unchanged, the
  zeroing when `allocate_zeroed` is not set, and the refusal to forward to
  the same copy (by `owner` and by address).
- `examples/embedded_d3d9_min/main.c`: a host with a different memory
  manager (4.2), a private Win32 heap and a lock, whose four functions have
  the signatures from section 1. It checks that the pointer from
  `GUI_MALLOC` is the pointer its `allocate` returned, and that its live
  count returns to zero after meek-ui's shutdown.
