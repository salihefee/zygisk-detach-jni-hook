#include "../zygisk/jni/detach_request.hpp"
#include <assert.h>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
static void integer(Bytes& b, uint32_t value) {
    for (int i = 0; i < 4; ++i) b.push_back(value >> (i * 8));
}
static void string16(Bytes& b, const std::string& value) {
    integer(b, value.size());
    for (char c : value) { b.push_back(c); b.push_back(0); }
    b.push_back(0); b.push_back(0);
    while (b.size() % 4) b.push_back(0);
}
static Bytes request(const std::string& package, size_t header = 12) {
    Bytes b(header);
    string16(b, "android.content.pm.IPackageManager");
    string16(b, package);
    integer(b, 0); // userId
    return b;
}
static Bytes rule(const std::string& package) {
    Bytes b{uint8_t(package.size() * 2 - 1)};
    for (char c : package) { b.push_back(c); b.push_back(0); }
    b.pop_back();
    return b;
}
static uint8_t* match(Bytes& b, const Bytes& rules, size_t header = 12) {
    return detachedPackageInRequest(b.data(), b.size(), header, rules.data(), rules.size());
}
int main() {
    // All supported encoded lengths, rather than hardcoded device packages.
    for (size_t header : {size_t(4), size_t(8), size_t(12)}) {
        for (size_t length = 1; length <= 128; ++length) {
            std::string name = "a" + std::string(length - 1, 'x');
            auto b = request(name, header), rules = rule(name);
            auto p = match(b, rules, header);
            assert(p && p[0] == 'a' && p[1] == 0);
            const size_t offset = p - b.data();
            for (size_t n = 0; n < offset + (length + 1) * 2; ++n)
                assert(!detachedPackageInRequest(b.data(), n, header, rules.data(), rules.size()));
            p[0] = p[1] = 0;
            assert(!match(b, rules, header));
        }
    }
    auto rules = rule("example.one");
    auto second = rule("org.example.two");
    rules.insert(rules.end(), second.begin(), second.end());
    auto b = request("org.example.two");
    assert(match(b, rules));
    auto unrelated = request("org.example.other");
    const auto before = unrelated;
    assert(!match(unrelated, rules) && unrelated == before);
    auto wrongInterface = b;
    wrongInterface[16] = 'X';
    assert(!match(wrongInterface, rules));
    auto oversizedDescriptor = b;
    for (int i = 12; i < 16; ++i) oversizedDescriptor[i] = 0xff;
    assert(!match(oversizedDescriptor, rules));
    auto truncated = rule("org.example.two");
    truncated.pop_back();
    assert(!match(b, truncated));
    auto even = rule("org.example.two");
    --even[0];
    assert(!match(b, even));
    auto longName = request(std::string(129, 'a'));
    assert(!match(longName, rules));
    assert(!detachedPackageInRequest(nullptr, 0, 12, rules.data(), rules.size()));
    assert(!detachedPackageInRequest(b.data(), b.size(), 12, nullptr, 0));
}
