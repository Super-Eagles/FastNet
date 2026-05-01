#pragma once

#include <sstream>
#include <stdexcept>
#include <string>

namespace FastNetTest {

inline void require(bool condition,
                    const char* expression,
                    const char* file,
                    int line,
                    const std::string& message = std::string()) {
    if (condition) {
        return;
    }

    std::ostringstream output;
    output << file << ':' << line << " assertion failed: " << expression;
    if (!message.empty()) {
        output << " (" << message << ')';
    }
    throw std::runtime_error(output.str());
}

template<typename Left, typename Right>
inline void requireEqual(const Left& left,
                         const Right& right,
                         const char* leftExpr,
                         const char* rightExpr,
                         const char* file,
                         int line) {
    if (left == right) {
        return;
    }

    std::ostringstream output;
    output << file << ':' << line << " assertion failed: " << leftExpr << " == " << rightExpr;
    throw std::runtime_error(output.str());
}

} // namespace FastNetTest

#define FASTNET_TEST_ASSERT(expr) \
    ::FastNetTest::require((expr), #expr, __FILE__, __LINE__)

#define FASTNET_TEST_ASSERT_MSG(expr, message) \
    ::FastNetTest::require((expr), #expr, __FILE__, __LINE__, (message))

#define FASTNET_TEST_ASSERT_EQ(left, right) \
    ::FastNetTest::requireEqual((left), (right), #left, #right, __FILE__, __LINE__)
