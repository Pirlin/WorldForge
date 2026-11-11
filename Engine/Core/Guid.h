#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace worldforge {

class Guid {
public:
    static std::optional<Guid> parse(std::string_view text);
    static Guid generate();

    bool isNull() const;
    std::string toString() const;
    auto operator<=>(const Guid &) const = default;

private:
    std::array<std::uint8_t, 16> bytes_{};
};

}
