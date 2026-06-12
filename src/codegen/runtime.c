// Рантайм LLVM-бэкенда. Линкуется к сгенерированному .ll через clang.
// Все функции зовутся из IR по именам @herta_*. Строки — пара
// { int64_t len; const char* data; }, передаются по значению.
// Память: строковые буферы выделяются через malloc и не освобождаются
// (используется арена-модель).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t len;
    const char* data;
} herta_string;

void herta_print_int(int64_t v)    { printf("%lld\n", (long long)v); }
void herta_print_uint(uint64_t v)  { printf("%llu\n", (unsigned long long)v); }
void herta_print_float(double v)   { printf("%g\n", v); }
void herta_print_bool(int8_t v)    { puts(v ? "true" : "false"); }

void herta_print_char(uint32_t cp) {
    // кодируем code point в UTF-8 и добавляем перевод строки
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

// Читает следующее целое из data[*pos_inout..total_len], пропуская пробелы.
// Возвращает 1, если число найдено (out_value заполнен, *pos_inout сдвинут),
// 0 — если чисел в строке больше нет.
int32_t herta_next_int(const char* data, int64_t total_len,
                       int32_t* pos_inout, int32_t* out_value) {
    int32_t p = *pos_inout;
    while (p < total_len) {
        char c = data[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++p;
        else break;
    }
    if (p >= total_len) return 0;
    int32_t sign = 1;
    if (data[p] == '-') { sign = -1; ++p; }
    else if (data[p] == '+') { ++p; }
    if (p >= total_len || data[p] < '0' || data[p] > '9') return 0;
    int32_t v = 0;
    while (p < total_len && data[p] >= '0' && data[p] <= '9') {
        v = v * 10 + (data[p] - '0');
        ++p;
    }
    *pos_inout = p;
    *out_value = v * sign;
    return 1;
}

herta_string herta_input(void) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        herta_string r = { 0, "" };
        return r;
    }
    // обрезаем завершающий перевод строки
    if (n > 0 && line[n - 1] == '\n') { line[n - 1] = '\0'; --n; }
    herta_string r = { (int64_t)n, line };
    return r;
}

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

void herta_rt_null_deref(int64_t line) {
    fflush(stdout);
    fprintf(stderr, "runtime error: null pointer dereference at line %lld\n",
            (long long)line);
    exit(1);
}
