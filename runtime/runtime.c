#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

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
    printf("%s", s);
}

void flint_println_str(const char* s) {
    printf("%s\n", s);
}

void flint_print_fmt(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void flint_panic(const char* msg) {
    fprintf(stderr, "PANIC: %s\n", msg);
    abort();
}

void flint_bounds_check(int64_t index, int64_t length) {
    if (index < 0 || index >= length) {
        fprintf(stderr, "PANIC: index %ld out of bounds for length %ld\n",
                (long)index, (long)length);
        abort();
    }
}

// ---- Self-hosting runtime ----

int64_t flint_arg_count(void) {
    return flint_g_argc;
}

char* flint_get_arg(int64_t i) {
    if (i < 0 || i >= flint_g_argc) return NULL;
    return flint_g_argv[i];
}

char* flint_str_concat(const char* a, const char* b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char* r = (char*)malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

int64_t flint_str_compare(const char* a, const char* b) {
    return (int64_t)strcmp(a, b);
}

int64_t flint_str_length(const char* s) {
    return (int64_t)strlen(s);
}

int64_t flint_str_char_at(const char* s, int64_t i) {
    return (unsigned char)s[i];
}

char* flint_str_substring(const char* s, int64_t start, int64_t end) {
    int64_t len = end - start;
    char* r = (char*)malloc((size_t)len + 1);
    memcpy(r, s + start, (size_t)len);
    r[len] = '\0';
    return r;
}

char* flint_i64_to_string(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return strdup(buf);
}

char* flint_file_read(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* content = (char*)malloc((size_t)len + 1);
    if (content) {
        fread(content, 1, (size_t)len, f);
        content[len] = '\0';
    }
    fclose(f);
    return content;
}

void flint_file_write(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}
