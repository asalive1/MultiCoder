#include <catch2/catch_test_macros.hpp>
#include <string>

static std::string extractTag(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t s = xml.find(open);
    if (s == std::string::npos) return "";
    s += open.size();
    size_t e = xml.find(close, s);
    if (e == std::string::npos) return "";
    return xml.substr(s, e - s);
}

static std::string applyTemplate(const std::string& tmpl, const std::string& artist, const std::string& title, const std::string& stationId) {
    std::string out = tmpl;
    auto repl = [&](const std::string& k, const std::string& v) {
        size_t p = 0;
        while ((p = out.find(k, p)) != std::string::npos) {
            out.replace(p, k.size(), v);
            p += v.size();
        }
    };
    repl("{artist}", artist);
    repl("{title}", title);
    repl("{stationId}", stationId);
    return out;
}

TEST_CASE("metadata XML field extraction", "[metadata]") {
    const std::string xml = R"(<item><artist>R.E.M</artist><title>Shiny Happy People</title><duration>210</duration></item>)";
    CHECK(extractTag(xml, "artist") == "R.E.M");
    CHECK(extractTag(xml, "title") == "Shiny Happy People");
    CHECK(extractTag(xml, "duration") == "210");
    CHECK(extractTag(xml, "missing").empty());
}

TEST_CASE("template rendering includes station id token", "[metadata]") {
    const std::string out = applyTemplate("artist={artist} | title={title} | {stationId}", "Beck", "Where It's At", "Encoder-1");
    CHECK(out == "artist=Beck | title=Where It's At | Encoder-1");
}

TEST_CASE("template rendering can omit station id", "[metadata]") {
    const std::string out = applyTemplate("{artist} - {title}", "Interpol", "C'mere", "Encoder-1");
    CHECK(out == "Interpol - C'mere");
}
