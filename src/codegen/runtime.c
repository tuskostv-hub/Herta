// Runtime для LLVM-бэкенда Herta.
// Линкуется к сгенерированному .ll через clang. Все функции вызываются из
// IR через obvious-имена `@herta_*`. Строки представлены структурой
// { int64_t len; const char* data; } и передаются/возвращаются по значению —
// clang применяет System V ABI lowering, на стороне Herta-IR используется
// та же {i64, ptr} структура.
//
// Память: строковые буферы аллоцируются malloc'ом и не освобождаются —
// арена-модель (semantics.md §5).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t len;
    const char* data;
} herta_string;

// ---------------------------------------------------------------------------
// Print
// ---------------------------------------------------------------------------

void herta_print_int(int64_t v)    { printf("%lld\n", (long long)v); }
void herta_print_uint(uint64_t v)  { printf("%llu\n", (unsigned long long)v); }
void herta_print_float(double v)   { printf("%g\n", v); }
void herta_print_bool(int8_t v)    { puts(v ? "true" : "false"); }

void herta_print_char(uint32_t cp) {
    // UTF-8 encode + newline.
    char buf[5];
    int n = 0;
    if (cp <= 0x7F) {
        buf[n++] = (char)cp;
    } else if (cp <= 0x7FF) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    buf[n] = '\0';
    puts(buf);
}

void herta_print_string(herta_string s) {
    fwrite(s.data, 1, (size_t)s.len, stdout);
    fputc('\n', stdout);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

int64_t herta_string_len(herta_string s) { return s.len; }

herta_string herta_string_concat(herta_string a, herta_string b) {
    int64_t n = a.len + b.len;
    char* buf = (char*)malloc((size_t)n + 1);
    if (a.len) memcpy(buf, a.data, (size_t)a.len);
    if (b.len) memcpy(buf + a.len, b.data, (size_t)b.len);
    buf[n] = '\0';
    herta_string r = { n, buf };
    return r;
}

int8_t herta_string_eq(herta_string a, herta_string b) {
    if (a.len != b.len) return 0;
    return memcmp(a.data, b.data, (size_t)a.len) == 0 ? 1 : 0;
}

herta_string herta_input(void) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        herta_string r = { 0, "" };
        return r;
    }
    // Strip trailing newline.
    if (n > 0 && line[n - 1] == '\n') { line[n - 1] = '\0'; --n; }
    herta_string r = { (int64_t)n, line };
    return r;
}

// ---------------------------------------------------------------------------
// Builtins управления выполнением
// ---------------------------------------------------------------------------

void herta_exit(int64_t code) {
    fflush(stdout);
    exit((int)code);
}

void herta_panic(herta_string msg) {
    fflush(stdout);
    fputs("panic: ", stderr);
    fwrite(msg.data, 1, (size_t)msg.len, stderr);
    fputc('\n', stderr);
    exit(1);
}

void herta_assert_fail(int64_t line) {
    fflush(stdout);
    fprintf(stderr, "assertion failed at line %lld\n", (long long)line);
    exit(1);
}

// ---------------------------------------------------------------------------
// Runtime errors
// ---------------------------------------------------------------------------

void herta_rt_div_zero(int64_t line) {
    fflush(stdout);
    fprintf(stderr, "runtime error: division by zero at line %lld\n",
            (long long)line);
    exit(1);
}

void herta_rt_oob(int64_t idx, int64_t size, int64_t line) {
    fflush(stdout);
    fprintf(stderr,
            "runtime error: index out of bounds: %lld, size %lld at line %lld\n",
            (long long)idx, (long long)size, (long long)line);
    exit(1);
}
