#include "common/datetime.h"
#include <cstdio>
#include <cstring>
#include <limits>

namespace minidb {

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool parse_ndigits(const char*& p, int n, int* out) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        if (!is_digit(p[i])) return false;
        value = value * 10 + (p[i] - '0');
    }
    p += n;
    *out = value;
    return true;
}

static bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month - 1];
}

// Howard Hinnant's civil calendar conversion, adjusted to Unix epoch.
static i64 days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<i64>(era) * 146097 + static_cast<i64>(doe) - 719468;
}

static void civil_from_days(i64 z, int* year, unsigned* month, unsigned* day) {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    *year = y;
    *month = m;
    *day = d;
}

bool datetime_parse_utc(const char* text, i64* out_micros) {
    if (!text || !out_micros) return false;
    const char* p = text;
    int year = 0, month = 0, day = 0;
    if (!parse_ndigits(p, 4, &year) || *p++ != '-' ||
        !parse_ndigits(p, 2, &month) || *p++ != '-' ||
        !parse_ndigits(p, 2, &day)) {
        return false;
    }
    if (year < 1 || month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month)) {
        return false;
    }

    int hour = 0, minute = 0, second = 0, micros = 0;
    if (*p != '\0') {
        if (*p != ' ' && *p != 'T') return false;
        p++;
        if (!parse_ndigits(p, 2, &hour) || *p++ != ':' ||
            !parse_ndigits(p, 2, &minute) || *p++ != ':' ||
            !parse_ndigits(p, 2, &second)) {
            return false;
        }
        if (hour > 23 || minute > 59 || second > 59) return false;
        if (*p == '.') {
            p++;
            int digits = 0;
            while (is_digit(*p)) {
                if (digits < 6) micros = micros * 10 + (*p - '0');
                digits++;
                p++;
            }
            if (digits == 0 || digits > 6) return false;
            while (digits++ < 6) micros *= 10;
        }
    }
    if (*p != '\0') return false;

    i64 days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    i64 seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    if (seconds > std::numeric_limits<i64>::max() / 1000000 ||
        seconds < std::numeric_limits<i64>::min() / 1000000) {
        return false;
    }
    *out_micros = seconds * 1000000 + micros;
    return true;
}

String datetime_format_utc(i64 micros) {
    i64 seconds = micros / 1000000;
    i64 usec = micros % 1000000;
    if (usec < 0) {
        usec += 1000000;
        seconds--;
    }
    i64 days = seconds / 86400;
    i64 sod = seconds % 86400;
    if (sod < 0) {
        sod += 86400;
        days--;
    }

    int year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, &year, &month, &day);
    int hour = static_cast<int>(sod / 3600);
    int minute = static_cast<int>((sod % 3600) / 60);
    int second = static_cast<int>(sod % 60);

    char buf[40];
    if (usec == 0) {
        int len = std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d:%02d",
                                year, month, day, hour, minute, second);
        return String(buf, static_cast<String::size_type>(len));
    }

    char frac[7];
    std::snprintf(frac, sizeof(frac), "%06lld", static_cast<long long>(usec));
    int frac_len = 6;
    while (frac_len > 0 && frac[frac_len - 1] == '0') frac_len--;
    int len = std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d:%02d.%.*s",
                            year, month, day, hour, minute, second, frac_len, frac);
    return String(buf, static_cast<String::size_type>(len));
}

} // namespace minidb
