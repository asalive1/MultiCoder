#include <catch2/catch_test_macros.hpp>
#include <string>

// Mirror of the production decodeXmlEntities in worker.cpp — kept in sync manually.
static std::string decodeXmlEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '&') { out.push_back(s[i++]); continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) { out.push_back(s[i++]); continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if      (ent == "amp")  { out.push_back('&');  i = semi + 1; }
        else if (ent == "apos") { out.push_back('\''); i = semi + 1; }
        else if (ent == "quot") { out.push_back('"');  i = semi + 1; }
        else if (ent == "lt")   { out.push_back('<');  i = semi + 1; }
        else if (ent == "gt")   { out.push_back('>');  i = semi + 1; }
        else if (!ent.empty() && ent[0] == '#') {
            try {
                unsigned long cp = 0;
                if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                    cp = std::stoul(ent.substr(2), nullptr, 16);
                else
                    cp = std::stoul(ent.substr(1), nullptr, 10);
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(s[i++]); continue;
                }
                i = semi + 1;
            } catch (...) { out.push_back(s[i++]); }
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

static std::string extractTag(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t s = xml.find(open);
    if (s == std::string::npos) return "";
    s += open.size();
    size_t e = xml.find(close, s);
    if (e == std::string::npos) return "";
    return decodeXmlEntities(xml.substr(s, e - s));
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

TEST_CASE("entity decode named entities in field values", "[metadata]") {
    // &apos; -> apostrophe (the original bug report case)
    const std::string xml1 = "<trivia>1969 - Billboard&apos;s Top Singles</trivia>";
    CHECK(extractTag(xml1, "trivia") == "1969 - Billboard's Top Singles");

    // &amp; -> &
    const std::string xml2 = "<title>Rock &amp; Roll</title>";
    CHECK(extractTag(xml2, "title") == "Rock & Roll");

    // &quot; -> "
    const std::string xml3 = "<title>Say &quot;Yeah&quot;</title>";
    CHECK(extractTag(xml3, "title") == "Say \"Yeah\"");

    // &lt; and &gt; are unusual in metadata but must decode correctly
    const std::string xml4 = "<title>A &lt; B &gt; C</title>";
    CHECK(extractTag(xml4, "title") == "A < B > C");
}

TEST_CASE("entity decode numeric character references", "[metadata]") {
    // &#39; is decimal apostrophe — some automation systems use this
    const std::string xml1 = "<trivia>Billboard&#39;s Top 40</trivia>";
    CHECK(extractTag(xml1, "trivia") == "Billboard's Top 40");

    // &#x27; is hex apostrophe
    const std::string xml2 = "<trivia>Billboard&#x27;s Hot 100</trivia>";
    CHECK(extractTag(xml2, "trivia") == "Billboard's Hot 100");
}

TEST_CASE("entity decode unknown entities passed through unchanged", "[metadata]") {
    // &nbsp; is not a standard XML entity — must not be silently dropped
    const std::string xml = "<title>Hello&nbsp;World</title>";
    CHECK(extractTag(xml, "title") == "Hello&nbsp;World");
}
