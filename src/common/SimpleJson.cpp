#include "../include/SimpleJson.h"

#include <cctype>
#include <sstream>

namespace simplejson {

namespace {

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool esc = false;
    for (char c : s) {
        if (esc) {
            switch (c) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(c); break;
            }
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

}  // namespace

bool Object::parse(const std::string& jsonText) {
    values_.clear();
    std::string t = trim(jsonText);
    if (t.size() < 2 || t.front() != '{' || t.back() != '}') {
        return false;
    }

    size_t i = 1;
    while (i < t.size() - 1) {
        while (i < t.size() - 1 && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == ',')) ++i;
        if (i >= t.size() - 1) break;
        if (t[i] != '"') return false;

        // key
        ++i;
        size_t kstart = i;
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < t.size()) i += 2;
            else ++i;
        }
        if (i >= t.size()) return false;
        std::string key = unescape(t.substr(kstart, i - kstart));
        ++i;

        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        if (i >= t.size() || t[i] != ':') return false;
        ++i;
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
        if (i >= t.size()) return false;

        Entry entry;
        if (t[i] == '"') {
            entry.quoted = true;
            ++i;
            size_t vstart = i;
            while (i < t.size() && t[i] != '"') {
                if (t[i] == '\\' && i + 1 < t.size()) i += 2;
                else ++i;
            }
            if (i >= t.size()) return false;
            entry.raw = unescape(t.substr(vstart, i - vstart));
            ++i;
        } else {
            size_t vstart = i;
            int depth = 0;
            bool inString = false;
            while (i < t.size() - 1) {
                char c = t[i];
                if (c == '"' && (i == vstart || t[i - 1] != '\\')) {
                    inString = !inString;
                } else if (!inString) {
                    if (c == '{' || c == '[') ++depth;
                    else if (c == '}' || c == ']') {
                        if (depth == 0) break;
                        --depth;
                    } else if (c == ',' && depth == 0) {
                        break;
                    }
                }
                ++i;
            }
            entry.raw = trim(t.substr(vstart, i - vstart));
            entry.quoted = false;
        }

        values_[key] = entry;
    }
    return true;
}

std::string Object::getString(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (it->second.quoted) return it->second.raw;
    return fallback;
}

int Object::getInt(const std::string& key, int fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stoi(it->second.quoted ? it->second.raw : trim(it->second.raw));
    } catch (...) {
        return fallback;
    }
}

double Object::getDouble(const std::string& key, double fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stod(it->second.quoted ? it->second.raw : trim(it->second.raw));
    } catch (...) {
        return fallback;
    }
}

bool Object::getBool(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    std::string v = trim(it->second.raw);
    if (it->second.quoted) v = it->second.raw;
    if (v == "true" || v == "1" || v == "yes") return true;
    if (v == "false" || v == "0" || v == "no") return false;
    return fallback;
}

bool Object::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::string Object::getRawValue(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    return it->second.raw;
}

Object Object::getSubObject(const std::string& key) const {
    Object result;
    auto it = values_.find(key);
    if (it != values_.end() && !it->second.quoted) {
        result.parse(it->second.raw);
    }
    return result;
}

void Object::setString(const std::string& key, const std::string& value) {
    values_[key] = Entry{value, true};
}

void Object::setInt(const std::string& key, int value) {
    values_[key] = Entry{std::to_string(value), false};
}

void Object::setBool(const std::string& key, bool value) {
    values_[key] = Entry{value ? "true" : "false", false};
}

void Object::setRaw(const std::string& key, const std::string& rawJsonValue) {
    values_[key] = Entry{trim(rawJsonValue), false};
}

std::string Object::serialize() const {
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& kv : values_) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << escape(kv.first) << "\":";
        if (kv.second.quoted) ss << "\"" << escape(kv.second.raw) << "\"";
        else ss << kv.second.raw;
    }
    ss << "}";
    return ss.str();
}

}  // namespace simplejson
