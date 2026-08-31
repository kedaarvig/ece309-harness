/* ============================================================
 * harness.c
 *
 * ECE 309 Project 1 - LLM Mini-Harness
 *
 * This file was produced through "vibe coding": the state machine,
 * data structures, and hard constraints below were specified by the
 * author BEFORE any code was written, then handed to an AI assistant
 * (Claude) to implement. See vibe_coding_log.md for the exact prompts
 * and iteration history that produced this file.
 *
 * Hard constraints given to the AI:
 *   - Standard C (C99) only. No POSIX-only or GNU-only extensions
 *     (no strdup, no strcasestr, no getline).
 *   - Only <stdio.h>, <stdlib.h>, <string.h>, <ctype.h>.
 *   - No third-party libraries.
 *   - Every non-trivial block gets a comment explaining *why*, not
 *     just *what*.
 *
 * State machine:
 *   INIT     -> allocate a Context, print a welcome banner
 *   PROMPT   -> print "You: ", read one line with fgets
 *   DISPATCH -> hand the line to mock_model(), which may invoke the
 *               calculator tool, then print the response
 *   (loop PROMPT/DISPATCH until the user types "exit", or stdin hits EOF)
 *   SHUTDOWN -> free the Context, print a goodbye message, exit(0)
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- configuration constants -------------------------------------- */
#define MAX_HISTORY      5      /* "last 5 turns" per the project spec  */
#define INPUT_BUFSIZE    512    /* max characters read from the user    */
#define RESPONSE_BUFSIZE 1024   /* max characters the mock model emits  */

/* ---- context management --------------------------------------------
 * A Turn stores who spoke ("user" or "model") and what was said.
 * `role` always points to a string literal, so it is never freed.
 * `text` is heap-allocated because its length isn't known up front (it
 * comes from user input, or from a formatted model response), so it
 * must be malloc'd and later free'd. That's the "allocate and manage
 * memory safely" requirement from the spec.
 * ------------------------------------------------------------------ */
typedef struct {
    const char *role;   /* "user" or "model" -- static, never freed   */
    char       *text;   /* heap copy of what was said -- must be freed */
} Turn;

typedef struct {
    Turn turns[MAX_HISTORY]; /* fixed-size ring buffer of turns        */
    int  count;              /* number of valid turns, capped at MAX   */
    int  next;               /* ring-buffer index of the next write    */
} Context;

/* Duplicate a string onto the heap. Standard C has no strdup() (it's
 * POSIX), so this is a small hand-rolled replacement. Returns NULL on
 * allocation failure. */
static char *str_dup(const char *src) {
    size_t len = strlen(src) + 1;           /* +1 for the null terminator */
    char *copy = (char *)malloc(len);
    if (copy != NULL) {
        memcpy(copy, src, len);
    }
    return copy;
}

/* Allocate and zero-initialize a new conversation Context. Exits with
 * an error message if the allocation fails -- there's nothing useful
 * the harness can do without its context buffer. */
static Context *context_create(void) {
    Context *ctx = (Context *)malloc(sizeof(Context));
    if (ctx == NULL) {
        fprintf(stderr, "harness: out of memory creating context\n");
        exit(EXIT_FAILURE);
    }
    memset(ctx, 0, sizeof(Context));
    return ctx;
}

/* Record one turn of the conversation. If the ring buffer slot about
 * to be written already holds a turn (i.e. the buffer is full and
 * we're wrapping around), its heap text is freed first. That's what
 * keeps memory bounded no matter how long the conversation runs --
 * and exactly what a leak checker (see test.sh) is verifying. */
static void context_add_turn(Context *ctx, const char *role, const char *text) {
    Turn *slot = &ctx->turns[ctx->next];

    if (slot->text != NULL) {      /* slot previously held a turn */
        free(slot->text);
        slot->text = NULL;
    }

    slot->role = role;             /* string literal, no allocation needed */
    slot->text = str_dup(text);    /* heap copy owned by this slot */

    ctx->next = (ctx->next + 1) % MAX_HISTORY;
    if (ctx->count < MAX_HISTORY) {
        ctx->count++;
    }
}

/* Print the stored history, oldest turn first. Backs the "history"
 * debug command so the context window is actually observable instead
 * of just trusted to exist. */
static void context_print(const Context *ctx) {
    int i;
    int start = (ctx->count < MAX_HISTORY) ? 0 : ctx->next;

    if (ctx->count == 0) {
        printf("(no turns stored yet)\n");
        return;
    }

    for (i = 0; i < ctx->count; i++) {
        int idx = (start + i) % MAX_HISTORY;
        printf("  [%d] %-5s: %s\n", i + 1, ctx->turns[idx].role, ctx->turns[idx].text);
    }
}

/* Free every heap-allocated turn, then free the Context itself.
 * Called exactly once, on the SHUTDOWN path. */
static void context_free(Context *ctx) {
    int i;
    for (i = 0; i < MAX_HISTORY; i++) {
        free(ctx->turns[i].text);
        ctx->turns[i].text = NULL;
    }
    free(ctx);
}

/* ---- tool execution --------------------------------------------------
 * A minimal calculator tool. LLMs are notoriously unreliable at doing
 * arithmetic themselves, so the mock model "calls a tool" instead of
 * trying to compute the answer inline -- requirement #3 of the spec.
 * ------------------------------------------------------------------ */

/* Case-insensitive prefix check, hand-rolled because strncasecmp is
 * POSIX, not standard C. Returns 1 if `text` begins with `prefix`. */
static int starts_with_ci(const char *text, const char *prefix) {
    size_t i;
    for (i = 0; prefix[i] != '\0'; i++) {
        if (text[i] == '\0') return 0;
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)prefix[i])) {
            return 0;
        }
    }
    return 1;
}

/* Case-insensitive substring search, hand-rolled for the same reason
 * (strcasestr is a glibc extension, not standard C). Returns 1 if
 * `needle` occurs anywhere inside `haystack`. */
static int contains_ci(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0 || nlen > hlen) return 0;

    for (i = 0; i + nlen <= hlen; i++) {
        if (starts_with_ci(haystack + i, needle)) return 1;
    }
    return 0;
}

/* Parses "<number> <op> <number>" (e.g. "12 * 4") and evaluates it.
 * Returns 1 and writes the result into *result on success, 0 on a
 * parse error, unknown operator, or divide-by-zero. */
static int tool_calculate(const char *expr, double *result) {
    double a, b;
    char op;

    if (sscanf(expr, " %lf %c %lf", &a, &op, &b) != 3) {
        return 0;   /* couldn't find two operands and an operator */
    }

    switch (op) {
        case '+': *result = a + b; return 1;
        case '-': *result = a - b; return 1;
        case '*': *result = a * b; return 1;
        case '/':
            if (b == 0.0) return 0;   /* refuse to divide by zero */
            *result = a / b;
            return 1;
        default:
            return 0;   /* unrecognized operator */
    }
}

/* ---- the mock "LLM" ---------------------------------------------------
 * Stands in for a real model call -- no network access, per the spec
 * ("you don't need to call the LLM APIs"). It pattern-matches the
 * input and, when appropriate, delegates to a tool. `response` must
 * point to a caller-owned buffer of at least RESPONSE_BUFSIZE bytes.
 * ------------------------------------------------------------------ */
static void mock_model(const Context *ctx, const char *input, char *response, size_t response_size) {
    /* Tool trigger: "calc <expr>" or "calculate <expr>" */
    if (starts_with_ci(input, "calc ") || starts_with_ci(input, "calculate ")) {
        const char *space = strchr(input, ' ');
        const char *expr = (space != NULL) ? space + 1 : "";
        double result;

        if (tool_calculate(expr, &result)) {
            snprintf(response, response_size, "[tool:calculator] %s = %g", expr, result);
        } else {
            snprintf(response, response_size,
                     "[tool:calculator] couldn't parse '%s'. Try: calc 12 + 8", expr);
        }
        return;
    }

    /* Debug command: show the context window */
    if (strcmp(input, "history") == 0) {
        snprintf(response, response_size, "(showing conversation history above)");
        return;
    }

    /* Canned greeting */
    if (contains_ci(input, "hello")) {
        snprintf(response, response_size,
                 "Hello! I'm a mock model with a %d-turn memory. Try 'calc 6 * 7'.",
                 MAX_HISTORY);
        return;
    }

    /* Default: echo, referencing how much context is currently stored */
    snprintf(response, response_size,
             "You said: \"%s\" (I'm holding %d/%d turns in context)",
             input, ctx->count, MAX_HISTORY);
}

/* ---- core loop -------------------------------------------------------- */
int main(void) {
    char input[INPUT_BUFSIZE];
    char response[RESPONSE_BUFSIZE];
    Context *ctx = context_create();

    printf("==================================================\n");
    printf(" ECE 309 Mini-Harness (mock model, no network calls)\n");
    printf(" Type a message, 'history' to see context, or 'exit' to quit.\n");
    printf("==================================================\n");

    for (;;) {
        printf("You: ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;   /* EOF (e.g. piped input ran out) -> shut down cleanly */
        }

        input[strcspn(input, "\n")] = '\0';   /* strip the trailing newline */

        if (strcmp(input, "exit") == 0) {
            break;
        }

        context_add_turn(ctx, "user", input);

        if (strcmp(input, "history") == 0) {
            context_print(ctx);
        }

        mock_model(ctx, input, response, sizeof(response));
        printf("Model: %s\n", response);

        context_add_turn(ctx, "model", response);
    }

    printf("Shutting down. Goodbye!\n");
    context_free(ctx);
    return 0;
}
