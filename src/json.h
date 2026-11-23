// Minimal JSON reader (objects, arrays, strings, numbers, true/false/null) - used for
// LibreHardwareMonitor's data.json and the GitHub releases API.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal* get(const char* k) const {
        for (auto& p : obj) if (p.first == k) return &p.second;
        return nullptr;
    }
};

struct JParser {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool lit(const char* s) { size_t n = strlen(s); if (size_t(e - p) >= n && !memcmp(p, s, n)) { p += n; return true; } return false; }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                ++p;
                char c = *p++;
                if (c == 'n') out += '\n'; else if (c == 't') out += '\t'; else if (c == 'r') out += '\r';
                else if (c == 'b' || c == 'f') {}
                else if (c == 'u' && e - p >= 4) {
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) { char h = *p++; cp = cp * 16 + unsigned(isdigit(h) ? h - '0' : (tolower(h) - 'a' + 10)); }
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                } else out += c;
            } else out += *p++;
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool val(JVal& v) {
        if (++depth > 64) return false;
        ws();
        bool ok = false;
        if (p >= e) ok = false;
        else if (*p == '{') {
            v.t = JVal::Obj; ++p; ws();
            if (p < e && *p == '}') { ++p; ok = true; }
            else for (;;) {
                ws();
                std::string k;
                if (!str(k)) break;
                ws();
                if (p >= e || *p != ':') break;
                ++p;
                v.obj.emplace_back(std::move(k), JVal{});
                if (!val(v.obj.back().second)) break;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; ok = true; }
                break;
            }
        } else if (*p == '[') {
            v.t = JVal::Arr; ++p; ws();
            if (p < e && *p == ']') { ++p; ok = true; }
            else for (;;) {
                v.arr.emplace_back();
                if (!val(v.arr.back())) break;
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; ok = true; }
                break;
            }
        } else if (*p == '"') { v.t = JVal::Str; ok = str(v.str); }
        else if (lit("true")) { v.t = JVal::Bool; v.num = 1; ok = true; }
        else if (lit("false")) { v.t = JVal::Bool; ok = true; }
        else if (lit("null")) { ok = true; }
        else {
            char* end = nullptr;
            std::string tmp(p, size_t(std::min<ptrdiff_t>(e - p, 64)));
            v.num = strtod(tmp.c_str(), &end);
            if (end != tmp.c_str()) { v.t = JVal::Num; p += end - tmp.c_str(); ok = true; }
        }
        --depth;
        return ok;
    }
};


inline bool Parse(const std::string& text, JVal& out) {
    JParser p{text.data(), text.data() + text.size()};
    return p.val(out);
}

} // namespace json
