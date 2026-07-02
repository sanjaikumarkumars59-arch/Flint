#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

// CLI argument globals (set by compiler-generated code in main())
int64_t flint_g_argc = 0;
char** flint_g_argv = NULL;

void flint_print_i64(int64_t val) {
    printf("%ld", (long)val);
}

void flint_println_i64(int64_t val) {
    printf("%ld\n", (long)val);
}

void flint_print_str(const char* s) {
    // printf("%s", NULL) is undefined behavior; guard it.
    printf("%s", s ? s : "(null)");
}

void flint_println_str(const char* s) {
    printf("%s\n", s ? s : "(null)");
}

// NOTE: `fmt` must be a trusted compile-time literal produced by the code
// generator, never untrusted user input (format-string vulnerability).
void flint_print_fmt(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
void flint_print_fmt(const char* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void flint_panic(const char* msg) {
    fprintf(stderr, "PANIC: %s\n", msg ? msg : "(null)");
    abort();
}

void flint_bounds_check(int64_t index, int64_t length) {
    if (index < 0 || index >= length) {
        fprintf(stderr, "PANIC: index %ld out of bounds for length %ld\n",
                (long)index, (long)length);
        abort();
    }
}

// Free a heap string returned by any flint_str_* / flint_i64_to_string /
// flint_file_read function. Callers own the returned memory.
void flint_str_free(char* s) {
    free(s);
}

// ---- Self-hosting runtime ----

int64_t flint_arg_count(void) {
    return flint_g_argc;
}

char* flint_get_arg(int64_t i) {
    if (i < 0 || i >= flint_g_argc || !flint_g_argv) return NULL;
    return flint_g_argv[i];
}

char* flint_str_concat(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    // Overflow guard: la + lb + 1 must not wrap.
    if (la > SIZE_MAX - lb - 1) return NULL;
    char* r = (char*)malloc(la + lb + 1);
    if (!r) return NULL;
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

int64_t flint_str_compare(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    return (int64_t)strcmp(a, b);
}

int64_t flint_str_length(const char* s) {
    return s ? (int64_t)strlen(s) : 0;
}

int64_t flint_str_char_at(const char* s, int64_t i) {
    if (!s) flint_panic("str_char_at: null string");
    int64_t len = (int64_t)strlen(s);
    flint_bounds_check(i, len);
    return (unsigned char)s[i];
}

char* flint_str_substring(const char* s, int64_t start, int64_t end) {
    if (!s) return NULL;
    int64_t slen = (int64_t)strlen(s);
    // Validate ordering and bounds — reject inverted/out-of-range slices.
    if (start < 0 || end < start || end > slen) {
        flint_panic("str_substring: invalid range");
    }
    int64_t len = end - start;               // guaranteed >= 0
    char* r = (char*)malloc((size_t)len + 1);
    if (!r) return NULL;
    memcpy(r, s + start, (size_t)len);
    r[len] = '\0';
    return r;
}

char* flint_i64_to_string(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return strdup(buf); // may return NULL on OOM; caller may check
}

char* flint_file_read(const char* path) {
    if (!path) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }              // ftell error / unseekable
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    if ((unsigned long)len >= SIZE_MAX) { fclose(f); return NULL; } // overflow guard

    char* content = (char*)malloc((size_t)len + 1);
    if (!content) { fclose(f); return NULL; }

    size_t n = fread(content, 1, (size_t)len, f);
    if (ferror(f)) { free(content); fclose(f); return NULL; }
    content[n] = '\0';   // terminate at actual bytes read
    fclose(f);
    return content;
}

// Returns 0 on success, -1 on failure.
int flint_file_write(const char* path, const char* content) {
    if (!path) return -1;
    if (!content) content = "";
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    int ok = (fputs(content, f) >= 0);
    if (fclose(f) != 0) ok = 0;   // flush errors surface at close
    return ok ? 0 : -1;
}
