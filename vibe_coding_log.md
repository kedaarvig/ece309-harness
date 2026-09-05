# Vibe Coding Log — ECE 309 Project 1

**Author:** Kedaar Vignes
**AI assistant used:** Claude (Sonnet)
**Date:** Aug 31, 2026

This log documents the specification-driven development (SDD) process I used to
generate `harness.c` and `test.sh`. Per the assignment, the goal wasn't to hand
the AI a vague request and hope for the best — it was to nail down the
architecture and constraints myself *first*, then use the AI as a fast (but
literal-minded) implementer. This file is the paper trail for that.

## 1. Architectural rules I decided on before writing any prompt

Before opening a chat with the AI, I wrote down the state machine and the
constraints I wanted enforced, since the project spec is explicit that a vague
ask like "write a chat app in C" produces messy code:

- **State machine:** `INIT` (allocate context) → `PROMPT` (print `You:`, read
  one line with `fgets`) → `DISPATCH` (hand the line to a mock model function,
  print its response) → loop back to `PROMPT`, until the user types `exit` (or
  stdin hits EOF) → `SHUTDOWN` (free everything, print a goodbye, exit 0).
- **Hard constraints:** standard C only (`<stdio.h>`, `<stdlib.h>`,
  `<string.h>`, `<ctype.h>`), no POSIX-only or GNU-only functions (this
  matters — see the iteration notes below), no third-party libraries, compiles
  clean under `gcc harness.c -o harness` with no flags.
- **Context management:** cap the conversation history at 5 turns like the
  spec suggests, using a fixed-size ring buffer instead of an
  unbounded/growing list, so memory usage is flat no matter how long a session
  runs. Each turn's text has to be individually heap-allocated (its length
  isn't known ahead of time) and individually freed when its slot gets
  recycled or the program exits — this is the part I most wanted the AI to
  get right, since it's also the part a leak checker actually tests.
- **Tool execution:** one deliberately simple tool — a calculator that parses
  `<number> <operator> <number>` — because the spec's example (math) is the
  textbook case of something an LLM fakes and a tool actually solves.

## 2. The prompt I fed the AI

I gave the AI the rules above almost verbatim, plus the exact function
signatures I wanted (`context_create`, `context_add_turn`, `context_free`,
`mock_model`, `tool_calculate`) so I wasn't just describing behavior in
English and hoping the shape of the code matched what I could review. I also
told it explicitly: *"Comment every non-trivial block — I need to be able to
explain this code line by line if asked."*

I additionally asked for a `history` debug command that prints the stored
turns, so the context window is something I can actually see working instead
of just trusting it's there.

## 3. Issues I caught and sent back

The first pass the AI produced used `strdup()` for copying turn text into the
heap. That's a POSIX function, not standard C — it happened to compile fine
under gcc/WSL, but it violated the "standard C only" constraint I'd set, so I
sent it back and had it write a 4-line `str_dup()` helper with `malloc` +
`memcpy` instead. Same story for case-insensitive matching: the first draft
reached for `strcasestr`, which is a glibc extension, so I had it replace that
with a hand-rolled `contains_ci()` built on `tolower()`.

I also specifically asked what happens on `calc 5 / 0` — the first version let
it through and printed `inf`. I asked for that to be treated as a tool error
instead of silently returning a garbage-looking number, which is why
`tool_calculate()` now explicitly checks for a zero divisor and reports a
parse-style failure rather than computing anything.

## 4. Requesting the AI-generated test script

Per requirement #5, I used almost exactly the prompt template from the
project spec:

> "I have a compiled C program named harness. Write a very simple Bash script
> that pipes a deterministic sequence of inputs into the program, checks that
> its context/state management behaves the way the spec describes, and checks
> that it does not leak memory."

The script it produced pipes a fixed 5-line transcript (`hello`,
`calc 6 * 7`, `history`, a plain sentence, `exit`) into the compiled binary and
greps the transcript for the exact response each step should produce. For the
memory check, I don't have `valgrind` installed in the environment I tested
in, so I had it add a fallback: if `valgrind` isn't on the machine, rebuild
with `-fsanitize=address,undefined` and check the ASan/LSan output for errors
instead. Both paths report a single pass/fail per check and a non-zero exit
code if anything fails, so it can be reused as a gate later instead of eyeballed.

## 5. Verification

- `gcc -std=c99 -Wall -Wextra -O2 harness.c -o harness` — zero warnings.
- `gcc harness.c -o harness` (the exact command from the spec, no flags) —
  compiles and runs correctly.
- `bash test.sh` — all 5 state-management checks pass, and the
  AddressSanitizer/LeakSanitizer build reports no leaks and no errors across
  the same transcript.
- Manually ran the binary and tried `calc` with bad input (`calc banana`),
  divide by zero (`calc 5 / 0`), and hammering `history` before any turns are
  stored — all produce a message instead of a crash.

## 6. A bug found during real-world testing (WSL2)

After the initial pass looked clean in my sandbox, I ran the compiled binary
interactively in my actual WSL2/Ubuntu terminal and typed `exit` — and it
didn't quit. It fell through to the default echo response instead. Turned out
the input line I'd typed had a trailing space (`"exit "`), and the shutdown
check was a strict `strcmp(input, "exit") == 0`, so anything other than the
four exact characters `exit` failed the match.

I asked the AI to harden the input-trimming step: strip the newline *and* a
stray `\r` (in case a Windows-side terminal ever sends CRLF) *and* any
trailing spaces/tabs, using `isspace()` from `<ctype.h>`, before doing any
comparisons at all. Rebuilt, reran `test.sh`, confirmed `exit `, `exit\t`, and
plain `exit` all shut the program down cleanly now, and that AddressSanitizer
was still clean. This is exactly the kind of thing SDD is supposed to catch
before submission rather than during grading.

## 7. What's mine vs. what the AI wrote

The state machine, the memory-safety rules, the tool choice, the function
signatures, the specific bugs called out in section 3, and this log are mine.
`harness.c` and `test.sh` are the AI's implementation of that specification —
which is the whole point of the assignment: I'm the architect who wrote the
spec and checked the work, not someone who typed every line by hand.
