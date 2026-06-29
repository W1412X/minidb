// Unit tests for the custom container primitives (String, Vector).
// Focus on ordering/edge cases that are easy to get subtly wrong.
#include "container/string.h"
#include "container/vector.h"
#include <cassert>

using namespace minidb;

static void test_string_prefix_ordering() {
    // A prefix must order strictly before the longer string. A naive memcmp
    // over only min(len) bytes would report these as equal-ordering.
    String ab("ab");
    String abc("abc");
    String abcd("abcd");

    assert(ab < abc);
    assert(abc < abcd);
    assert(!(abc < ab));
    assert(!(abcd < abc));
    assert(abc > ab);
    assert(ab <= ab);
    assert(abc >= abc);
    assert(ab <= abc);
    assert(abcd >= abc);

    // Empty string orders before any non-empty string.
    String empty;
    assert(empty < ab);
    assert(!(ab < empty));

    // compare() must be antisymmetric and consistent with the operators.
    assert(ab.compare(abc) < 0);
    assert(abc.compare(ab) > 0);
    assert(abc.compare(abc) == 0);
}

static void test_string_sort_with_prefixes() {
    // Sorting strings where some are prefixes of others must be a total order.
    Vector<String> v;
    v.push_back(String("abcd"));
    v.push_back(String("a"));
    v.push_back(String("abc"));
    v.push_back(String("ab"));
    v.push_back(String("abce"));
    v.sort();
    assert(v.size() == 5);
    assert(v[0] == String("a"));
    assert(v[1] == String("ab"));
    assert(v[2] == String("abc"));
    assert(v[3] == String("abcd"));
    assert(v[4] == String("abce"));
    // Verify the result is non-decreasing under the operators.
    for (u32 i = 1; i < v.size(); i++) assert(v[i - 1] <= v[i]);
}

static void test_string_append_sso_to_heap() {
    // Crossing the SSO boundary via += must preserve all bytes.
    String s;
    for (int i = 0; i < 100; i++) s += 'x';
    assert(s.size() == 100);
    for (u32 i = 0; i < s.size(); i++) assert(s[i] == 'x');
    assert(s.c_str()[s.size()] == '\0');
}

static void test_vector_insert_erase() {
    Vector<int> v;
    for (int i = 0; i < 5; i++) v.push_back(i);   // 0 1 2 3 4
    v.insert(v.begin() + 2, 99);                  // 0 1 99 2 3 4
    assert(v.size() == 6 && v[2] == 99 && v[3] == 2);
    v.erase(v.begin() + 2);                        // 0 1 2 3 4
    assert(v.size() == 5 && v[2] == 2 && v.back() == 4);
    v.erase(v.begin() + 1, v.begin() + 3);         // 0 3 4
    assert(v.size() == 3 && v[0] == 0 && v[1] == 3 && v[2] == 4);
}

int main() {
    test_string_prefix_ordering();
    test_string_sort_with_prefixes();
    test_string_append_sso_to_heap();
    test_vector_insert_erase();
    return 0;
}
