#include <catch2/catch_test_macros.hpp>
#include "SimpleJson.h"

TEST_CASE("SimpleJson parse and getters", "[simplejson]") {
    simplejson::Object o;
    REQUIRE(o.parse("{\"user\":\"Admin\",\"firstLoginRequired\":true,\"uiPort\":8050}"));
    CHECK(o.getString("user", "") == "Admin");
    CHECK(o.getBool("firstLoginRequired", false));
    CHECK(o.getInt("uiPort", 0) == 8050);
}

TEST_CASE("SimpleJson setters and serialize", "[simplejson]") {
    simplejson::Object o;
    o.setString("adminUser", "Admin");
    o.setString("adminPass", "change-me");
    o.setBool("firstLoginRequired", true);
    o.setInt("uiPort", 8050);

    std::string json = o.serialize();
    simplejson::Object re;
    REQUIRE(re.parse(json));
    CHECK(re.getString("adminUser", "") == "Admin");
    CHECK(re.getString("adminPass", "") == "change-me");
    CHECK(re.getBool("firstLoginRequired", false));
    CHECK(re.getInt("uiPort", 0) == 8050);
}
