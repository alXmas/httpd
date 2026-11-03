# httpd: a small HTTP/1.1 server in C

A learning project: an HTTP server written from scratch in C, using only the
standard library and POSIX sockets. The goal is to understand what happens
between `socket()` and a page in the browser: short reads and writes, request
parsing, file serving, path confinement, processes and signals. The server should
also handle hostile or broken clients without crashing or leaking resources.

## Quick start

```sh
make                                   # build
./httpd --port 8080 --root ./www       # run
curl -v http://localhost:8080/         # check
make test                              # unit + HTTP tests
make LOG_LEVEL=0                       # build without debug logging
make SANITIZE=1 test                   # tests under ASan/UBSan
```

## Supported HTTP subset

| Supported                                       | Not supported (by design)                |
| :---------------------------------------------- | :--------------------------------------- |
| `GET` only                                      | `POST`, `PUT`, other methods → `405`     |
| HTTP/1.1 request line                           | Other versions → `505`                   |
| One request per connection, `Connection: close` | Keep-alive, pipelining                   |
| `Content-Length` in responses                   | Chunked encoding, request bodies → `400` |
| Static files from `--root`, flat names only     | Subdirectories, directory listing        |
| One whitelisted CGI script: `/cgi-bin/status`   | Arbitrary CGI, HTTPS, compression        |

Limits: request headers ≤ 8 KiB (`431`), request receive timeout 5 s,
CGI runtime ≤ 2 s (`504`), CGI output ≤ 64 KiB (`502`).

## Project layout

```text
src/
  main.c         # args, listening socket, accept loop, signals
  http_parse.c   # request parser (no sockets inside)
  fs.c           # confined file opening and sending
  cgi.c          # fork/exec/pipe for /cgi-bin/status
  log.c          # LOG_ERR / LOG_DBG
tests/
  unit/          # parser tests, no network
  http/          # Python tests sending raw bytes
fuzz/
  fuzz_parse.c   # libFuzzer entry point
  corpus/        # seed inputs + saved crashes
www/             # document root

```
---

## Roadmap

Legend: 🔧 the problem will show up on its own · 💥 break it on purpose in a
scratch branch, explain it, then fix it.

### 1. First page

- [ ] Accept a connection, read the request up to the empty line, reply with
      `200 OK`, `Content-Length`, `Connection: close` and a small HTML page.
- 🔧 Restarting gives `Address already in use`: check `TIME_WAIT` with `ss -tan`,
  then fix it with `SO_REUSEADDR`.
- **Done:** `curl -v` shows correct headers, the browser renders the page and the
  server restarts immediately.

### 2. Switchable logging

- [ ] `LOG_ERR` / `LOG_DBG` print the file, line, function and message, and also
      work with no extra arguments.
- [ ] With `make LOG_LEVEL=0`, debug calls and the evaluation of their arguments
      disappear. Verify with a side-effect counter and a unique string that
      `strings httpd` no longer finds.
- 💥 Use a two-statement macro inside `if/else`, find the problem in `cc -E`
  output and fix it with `do { } while (0)`.
- 💥 Call `#define MIN(a, b) a < b ? a : b` as `MIN(n++, limit) * 2`. Compare
  the result and the increment count with a `static inline` function.
- 🔧 Make does not rebuild when only `LOG_LEVEL` changes: use separate build
  directories or a flags file listed as a dependency.
- **Done:** both configurations build and tests check both of them.

### 3. Multiple modules

- [ ] Split the code into modules, one header per `.c`. Use `-MMD -MP` for header
      dependencies, plus `all`, `test` and `clean` targets.
- 💥 Put `int g_conn_count = 0;` in a header included from two `.c` files to get
  `multiple definition`, find both definitions with `nm`, then fix it with
  `extern`.
- 💥 Try `int g_conn_count;` with `-fcommon` vs `-fno-common` (the GCC 10+ default).
- 💥 Change a function signature in a `.c` that does not include its own header,
  watch it link anyway, then include the header so the compiler catches it.
- 💥 Include a header with a struct twice, then add include guards.
- [ ] Make internal functions `static`, then check `t` vs `T` in `nm`.
- **Done:** a clean build takes one command, and touching a header rebuilds only the
  objects that depend on it. Given an error message, you can tell whether it came
  from the compiler or the linker.

### 4. Parse requests, reject bad clients

- [ ] Return `400` for malformed requests, `405` + `Allow: GET` for other methods,
      `505` for other versions and `431` for oversized headers.
- [ ] Reject request bodies, `Content-Length` together with `Transfer-Encoding`
      and conflicting `Content-Length` values. Close the connection after
      responding.
- 🔧 A request can arrive one byte at a time: one `recv()` ≠ one request.
- [ ] If the status code table gets duplicated, switch it to an X-macro.
- **Done:** raw-byte tests cover every case, and parser unit tests run without
  a network.

### 5. Serve real files

- [ ] `GET /` → `www/index.html`, `GET /files/<name>` → a flat name inside `www`.
- [ ] Pick `Content-Type` by file extension, and get the size with `fstat` on the
      already opened fd.
- [ ] Send files in fixed-size chunks; `send()` may write less than requested.
- 💥 Ignore the return value of `send()` and download 100 MB with
  `curl --limit-rate 1M`, then compare `sha256sum`.
- ⭐ Compare with `sendfile(2)` using `strace -c`.
- **Done:** checksums match for empty, small and large files, RSS does not grow
  with file size, and a missing file gives `404`.

### 6. Try to escape `www`

- [ ] Create a canary file outside `www`. Write a naive string check first, then
      attack it with `..`, `%2e%2e`, `..%2f`, `%2F`, `%252e`, `%00` and a symlink
      pointing outside.
- [ ] Send raw paths from a Python socket or `curl --path-as-is`, because browsers
      and plain curl normalize `..` themselves.
- [ ] Replace the check with `openat2()` relative to the root fd, using
      `RESOLVE_BENEATH` (optionally with `RESOLVE_NO_SYMLINKS`). Call it through
      `syscall(SYS_openat2, ...)` if libc has no wrapper. Fallback: a single path
      component plus `O_NOFOLLOW`, documented as a limitation.
- **Done:** no test ever receives the canary bytes, normal files still work, and
  all attacks stay in `make test`.

### 7. Stop crashing and hanging

- [ ] Scenarios: the client disconnects mid-download, closes mid-header or never
      sends anything; a file is unreadable; the port is busy.
- 🔧 The process silently dies from `SIGPIPE` when a client disconnects: find it
  with `strace`, then use `MSG_NOSIGNAL` and handle `EPIPE`.
- 💥 Slowloris: a client that sends nothing blocks the single-threaded server.
  Add a receive timeout (`SO_RCVTIMEO` or `poll`). A full fix comes later
  with non-blocking I/O.
- 💥 With `SA_RESTART`, SIGTERM does not interrupt `accept`. Explain why, then
  remove it and check a `volatile sig_atomic_t` flag on `EINTR`. The handler
  must not call `printf`.
- [ ] Reproduce each memory bug with a small test, then debug it with GDB or ASan.
- **Done:** every scenario is tested, the server keeps serving after each one,
  and SIGTERM stops it within a bounded time.

### 8. CGI: a page generated by another program

- [ ] `GET /cgi-bin/status` runs one whitelisted script:
      `pipe` → `fork` → `dup2` stdout → `execve` with a minimal env
      (`REQUEST_METHOD`, `QUERY_STRING`, `PATH`). No `system()`, no shell.
- [ ] The script prints `Content-Type`, an empty line and the body. The server
      builds the HTTP response with `Content-Length`. A launch failure, non-zero
      exit or oversized output gives `502`; a timeout gives `504`.
- 🔧 Calling `waitpid` before reading deadlocks when the output exceeds the pipe
  buffer: read to EOF or the limit first, then reap the child.
- 💥 Leave the write end open in the parent: `read` never returns EOF.
- 💥 Skip `waitpid`: `ps` shows `<defunct>`.
- 💥 Create the listening socket without `SOCK_CLOEXEC`, then find it in
  `/proc/<script pid>/fd`.
- 🔧 Ignored signals and the signal mask survive `exec`, so reset them in the
  child.
- [ ] On timeout, `kill` the child and then always `waitpid`. Process groups are ⭐.
- **Done:** 1000 mixed requests (normal, hanging, crashing and verbose scripts,
  client disconnects) leave zero zombies and the original fd count.

### 9. Reproducible build and CI

- [ ] `make test` runs the parser unit tests and the Python HTTP tests against a
      live server.
- [ ] CI builds with `-Wall -Wextra -Werror`, runs the tests under ASan/UBSan
      (LSan included) and does a short fuzz run.
- [ ] libFuzzer calls `parse_request` directly. The seed corpus comes from the
      tests, and saved crashes become regression inputs. Fuzz locally for at
      least an hour.
- **Done:** CI is green from a clean clone, and any crash input reproduces with
  one command.

### Stage complete when

- The browser shows the page, and large files download intact.
- All scenarios above run automatically.
- 10,000 sequential requests mixed with disconnects and CGI calls leave the fd
  count equal to the start value, RSS flat after warm-up, and sanitizers and
  leak checks silent.
- The supported HTTP subset and run command are documented here.

### Extras

- [ ] Put the build version in the `Server:` header via `-DSERVER_VERSION=...`.
      Explain the escaped quotes and the `#` operator.

---

## Questions I should answer using my own code

- Why did the macro break `if/else`? Where does the preprocessor end and the
  compiler begin?
- Which errors come from the compiler and which from the linker? What do
  `static` and `extern` do?
- Why didn't Make rebuild after a flag change?
- Who owns the request buffer? Why is `recv` ≠ request and `send` ≠ response?
- How is every request confined to `www`, and which tests prove it?
- Why did a client disconnect kill the server? Why didn't SIGTERM interrupt
  `accept`?
- What does a child inherit through `fork`, and what survives `exec`?
- What keeps a pipe from reaching EOF? Where did the CGI deadlock come from? Who
  reaps the child after a timeout?
- Why can one slow client stall the whole server?

## Bug log format (`NOTES.md`)

```text
## <date> <short title>
Symptom: what I observed (command, output)
Cause:   what actually happened
Fix:     what changed and why it works
Test:    which test now covers it
```

---

## Learning materials

Open materials when a task gets stuck; you do not need to read whole books.

### Main guide

- **CS:APP**: Bryant & O'Hallaron, _Computer Systems: A Programmer's
  Perspective_, 3rd ed. Chapter 11 builds **Tiny**, a minimal web server with
  static content and CGI. Read it, write your own without copying, then list
  where Tiny is unsafe.
- **CMU 15-213 lectures** (recorded semesters with slides and video): Linking,
  Exceptional Control Flow, System-Level I/O, Network Programming.

### By task

| Task                          | Material                                                                                                                               |
| :---------------------------- | :------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Sockets, first page        | Beej's Guide to Network Programming; CS:APP ch. 11; Jacob Sorber's YouTube video on writing a web server in C                          |
| 2. Macros, logging            | GNU CPP manual: _Macro Pitfalls_, _Variadic Macros_, _Stringizing_; `cc -E`; Beej's Guide to C                                         |
| 3. Modules, linking, Make     | CS:APP ch. 7; GNU Make manual; Paul Smith, _Auto-Dependency Generation_; Gustedt, _Extreme C_ (Amini), early chapters                  |
| 4. Parsing, short reads       | RFC 9112 §2–3, §6, §9; CS:APP ch. 10 (Rio); `recv(2)`, `socket(7)`                                                                     |
| 5. File serving               | `open(2)`, `fstat(2)`, `send(2)`, `sendfile(2)`; TLPI ch. 4–5                                                                          |
| 6. Path confinement           | OWASP _Path Traversal_; `openat2(2)`, `path_resolution(7)`; TLPI ch. 18                                                                |
| 7. Signals, robustness        | TLPI ch. 20–22 and 61; `sigaction(2)`, `signal-safety(7)`, `socket(7)`                                                                 |
| 8. CGI                        | RFC 3875 (env variables, script output); CS:APP ch. 8; TLPI ch. 24–27, 44; `fork(2)`, `execve(2)`, `pipe(2)`, `waitpid(2)`, `fcntl(2)` |
| 9. Tests, sanitizers, fuzzing | Clang AddressSanitizer / UBSan docs; LLVM libFuzzer docs; google/fuzzing _libFuzzerTutorial_                                           |
| Debugging                     | GDB manual (Beej's Guide to GDB is a short intro); `strace`, `ss`, `lsof`, `/proc/<pid>/fd`                                            |

### References

- **TLPI**: Michael Kerrisk, _The Linux Programming Interface_: the main
  Linux reference.
- **Linux man-pages**: man7.org/linux/man-pages
- **Optional warm-up:** CodeCrafters _Build Your Own HTTP Server_ (C track,
  automated tests). It covers a different feature set: no security, CGI or
  robustness work.
