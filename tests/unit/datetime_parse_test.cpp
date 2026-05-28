#include "common/datetime.h"
#include <cstdlib>
#include <cstdio>

using namespace minidb;

static void expect_parse(const char* input, i64 micros, const char* formatted) {
    i64 actual = 0;
    if (!datetime_parse_utc(input, &actual) ||
        actual != micros ||
        datetime_format_utc(actual) != formatted) {
        std::fprintf(stderr, "datetime parse expectation failed for %s\n", input);
        std::abort();
    }
}

static void expect_reject(const char* input) {
    i64 actual = 0;
    if (datetime_parse_utc(input, &actual)) {
        std::fprintf(stderr, "datetime reject expectation failed for %s\n", input);
        std::abort();
    }
}

int main() {
    expect_parse("1970-01-01", 0, "1970-01-01 00:00:00");
    expect_parse("1970-01-01 00:00:00.000001", 1, "1970-01-01 00:00:00.000001");
    expect_parse("2026-05-25T20:15:30.5", 1779740130500000LL,
                 "2026-05-25 20:15:30.5");
    expect_parse("2000-02-29 23:59:59.123456", 951868799123456LL,
                 "2000-02-29 23:59:59.123456");

    expect_reject("");
    expect_reject("2026-02-29 00:00:00");
    expect_reject("2026-05-25 24:00:00");
    expect_reject("2026-05-25 20:15:30Z");
    expect_reject("2026-05-25 20:15:30.1234567");

    std::puts("datetime_parse_test passed");
    return 0;
}
