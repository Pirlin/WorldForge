#include "Engine/Core/Guid.h"

#include <algorithm>
#include <random>

namespace worldforge {
namespace {
int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
}

std::optional<Guid> Guid::parse(std::string_view text)
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
        return std::nullopt;

    Guid result;
    std::size_t cursor = 0;
    for (auto &byte : result.bytes_) {
        if (cursor == 8 || cursor == 13 || cursor == 18 || cursor == 23)
            ++cursor;
        const int high = hexDigit(text[cursor++]);
        const int low = hexDigit(text[cursor++]);
        if (high < 0 || low < 0)
            return std::nullopt;
        byte = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

Guid Guid::generate()
{
    std::random_device entropy;
    std::uniform_int_distribution<unsigned> byte(0, 255);
    Guid result;
    for (auto &value : result.bytes_)
        value = static_cast<std::uint8_t>(byte(entropy));
    result.bytes_[6] = static_cast<std::uint8_t>((result.bytes_[6] & 0x0f) | 0x40);
    result.bytes_[8] = static_cast<std::uint8_t>((result.bytes_[8] & 0x3f) | 0x80);
    return result;
}

bool Guid::isNull() const
{
    return std::ranges::all_of(bytes_, [](auto byte) { return byte == 0; });
}

std::string Guid::toString() const
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            result.push_back('-');
        const auto byte = bytes_[index];
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
}
