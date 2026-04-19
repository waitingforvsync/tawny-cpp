#include <doctest/doctest.h>

TEST_CASE("heavyweight example test") {
    SUBCASE("addition") {
        CHECK(2 + 2 == 4);
    }
    SUBCASE("subtraction") {
        CHECK(5 - 3 == 2);
    }
}
