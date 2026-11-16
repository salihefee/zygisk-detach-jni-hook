#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Return the first UTF-16 character of a configured package in an enabled-state
// request. All offsets are checked before touching the Parcel or config data.
inline uint8_t* detachedPackageInRequest(uint8_t* data, size_t size, size_t headerSize,
                                       const uint8_t* rules, size_t rulesSize) {
    constexpr char16_t descriptor[] = u"android.content.pm.IPackageManager";
    constexpr size_t descriptorLength = sizeof(descriptor) / sizeof(char16_t) - 1;
    if (!data || !rules || headerSize > size || size - headerSize < 4) return nullptr;

    uint32_t length;
    memcpy(&length, data + headerSize, 4);
    if (length != descriptorLength) return nullptr;
    size_t cursor = headerSize + 4;
    constexpr size_t descriptorBytes = sizeof(descriptor);
    if (size - cursor < descriptorBytes || memcmp(data + cursor, descriptor, descriptorBytes)) return nullptr;
    cursor = (cursor + descriptorBytes + 3) & ~size_t(3);
    if (cursor > size || size - cursor < 4) return nullptr;

    memcpy(&length, data + cursor, 4);
    cursor += 4;
    if (!length || length > 128 || size - cursor < (size_t(length) + 1) * 2) return nullptr;
    if (data[cursor + length * 2] || data[cursor + length * 2 + 1]) return nullptr;
    const size_t encodedLength = length * 2 - 1;
    for (size_t i = 0; i < rulesSize;) {
        const size_t count = rules[i++];
        if (!count || count > rulesSize - i || !(count & 1)) return nullptr;
        if (count == encodedLength && !memcmp(rules + i, data + cursor, count)) return data + cursor;
        i += count;
    }
    return nullptr;
}
