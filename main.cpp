#pragma once

#include <string>
#include <iostream>

class BigInt {
public:
    BigInt(long long value = 0);

    std::string to_string() const;

private:
    long long value_;
};

int main() {
  std::cout << "LOL" << std::endl;
}
