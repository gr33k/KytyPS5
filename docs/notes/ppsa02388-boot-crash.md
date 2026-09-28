# PPSA02388 (A Plague Tale: Innocence) boot crash forensics

100% reproducible crash ~12s into boot, after soundbank loads and Avplayer
`LOGO.MP4` setup. Build: fork main @ strip-expansion + materialization-diag work.

## Fault

```
Unhandled host exception: type=1 code=3221225477 pc=0x00000009007b1d66 access=2 address=0x0000000000000020
```

MainThread. Guest bytes verified 96/96 against `eboot.bin`:

```
mov rcx,[rdi] / mov rax,[rdi+0xb8] / mov r14,rdi
mov ebx,[rcx+0x50] / mov r12d,[rcx+0x54]      ; 0x780/0x438 (1920x1080)
test rax,rax / je null
mov edx,[rax+8] / cmp edx,[r14+0xc4] / jne null / mov rdx,[rax+0x18]
null: mov [rdx+0x20],ebx   <- FAULT with rdx=0
```

`rax = [obj+0xb8]` is NULL, and even the null path writes through it, so the
game assumes the entry list is always populated. Second list at `[obj+0xc8]`
is consumed immediately after (r13d/ecx halves written to it).

## Verified call chain

Caller (at `0x9007b0850`, return address `0x9007b08f8` confirmed via `e8`
decode) runs a count/query/type-filter enumeration loop over a handle at
`[obj+0x318]` (takes type==1, skips 2), stores dims, then calls the crashing
helper with `rdi = [obj+0x340]`.

## Ruled out (with log evidence)

- Avplayer stream path works: `StreamCount -> 2`, `GetStreamInfo(0) -> 0`,
  `GetStreamInfo(1) -> 0`, MP4 parses with no avformat errors.
- Zero unresolved imports in the crashing run.
- No graphics pipeline aborts before the fault (all create successfully,
  including the former AMD `ErrorUnknown` cases via strip expansion).
- All file I/O succeeds (banks, WWISEIDS, MP4 present and readable).
- Not a code-patch artifact: guest bytes match the file exactly.

## Open hypotheses

1. The `[obj+0xb8]` list is populated by a step whose HLE precondition
   differs under emulation (e.g. video-decoder-init path), leaving it null.
2. Async population race: a job thread fills the list after MainThread reads it.
3. The type-1 entry the loop seeks is never produced because a queried HLE
   value differs from hardware (dims? decoder caps?).

## Diagnostic aids in tree (temporary, remove before upstream PR)

- `[diag]` LOGF lines in `AvPlayerStreamCount`, `AvPlayerGetStreamInfo`,
  `AvPlayerAddSource` (`src/libs/avPlayer.cpp`) — PRINT_NAME is a per-TU
  thread-local and does not reliably trace, these do.
- `host backtrace` with module names on fatal errors
  (`src/loader/runtimeLinker.cpp`) — keeper, already pushed.

## Logs / artifacts

- Full boot log to fault: ~83k lines, ends in Wwise bank init + Avplayer setup.
- Repro: launch the game, wait ~12s, no input needed.
