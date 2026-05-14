#pragma once

#include <map>
#include <string>

namespace simplejson {

// Lightweight flat JSON helper for small config payloads.
// Supports string, bool, number fields and nested objects represented as raw JSON.
class Object {
public:
    bool parse(const std::string& jsonText);

    std::string getString(const std::string& key, const std::string& fallback = "") const;
    int getInt(const std::string& key, int fallback = 0) const;
    double getDouble(const std::string& key, double fallback = 0.0) const;
    bool getBool(const std::string& key, bool fallback = false) const;

    bool has(const std::string& key) const;
    // Returns the raw JSON token for key (unquoted: array/object/number; quoted: unescaped string).
    std::string getRawValue(const std::string& key, const std::string& fallback = "") const;
    // Parses a nested JSON object stored under key and returns it as an Object.
    Object getSubObject(const std::string& key) const;
    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setBool(const std::string& key, bool value);
    void setRaw(const std::string& key, const std::string& rawJsonValue);

    std::string serialize() const;

private:
    struct Entry {
        std::string raw;
        bool quoted = false;
    };
    std::map<std::string, Entry> values_;
};

}  // namespace simplejson
