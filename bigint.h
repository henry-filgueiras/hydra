#pragma once

#include <string>

class BigInt {
public:
    BigInt(long long value = 0);

    std::string to_string() const;

private:
    long long value_;
};
