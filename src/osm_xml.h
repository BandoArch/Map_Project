#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cerrno>

namespace osm {

class Tags {
public:
    using Read = int (*)(void *, uint8_t *, size_t);
    Tags(Read read, void *context) : read_(read), context_(context) {}

    void rewind() { used_ = position_ = 0; failed_ = false; }
    bool failed() const { return failed_; }

    bool next(char *tag, size_t capacity)
    {
        for (;;) {
            int c;
            do { c = get(); } while (c >= 0 && c != '<');
            if (c < 0) return false;
            size_t length = 0;
            int quote = 0;
            bool overflow = false;
            while ((c = get()) >= 0) {
                if (c == '>' && !quote) break;
                if (c == quote) quote = 0;
                else if (!quote && (c == '\'' || c == '"')) quote = c;
                if (length + 1 < capacity) tag[length++] = static_cast<char>(c);
                else overflow = true;
            }
            if (c < 0) { failed_ = true; return false; }
            tag[length] = '\0';
            // Long descriptions are irrelevant to geometry; consume the entire tag.
            if (!overflow && tag[0] != '?' && tag[0] != '!') return true;
        }
    }

private:
    int get()
    {
        if (position_ == used_) {
            const int count = read_(context_, buffer_, sizeof(buffer_));
            if (count <= 0) { if (count < 0) failed_ = true; return -1; }
            used_ = static_cast<size_t>(count);
            position_ = 0;
        }
        return buffer_[position_++];
    }
    Read read_;
    void *context_;
    uint8_t buffer_[2048];
    size_t used_ = 0, position_ = 0;
    bool failed_ = false;
};

inline bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline bool element(const char *tag, const char *name)
{
    const size_t n = std::strlen(name);
    return std::strncmp(tag, name, n) == 0 && (tag[n] == '\0' || tag[n] == '/' || space(tag[n]));
}

inline bool attribute(const char *tag, const char *name, char *value, size_t capacity)
{
    const char *p = tag;
    while (*p && !space(*p)) ++p;
    while (*p) {
        while (space(*p) || *p == '/') ++p;
        const char *key = p;
        while (*p && !space(*p) && *p != '=') ++p;
        const size_t length = static_cast<size_t>(p - key);
        while (space(*p)) ++p;
        if (*p != '=') return false;
        ++p;
        while (space(*p)) ++p;
        if (*p != '\'' && *p != '"') return false;
        const char quote = *p++;
        const char *start = p;
        while (*p && *p != quote) ++p;
        if (!*p) return false;
        if (length == std::strlen(name) && std::strncmp(key, name, length) == 0) {
            const size_t size = static_cast<size_t>(p - start);
            if (size + 1 > capacity) return false;
            std::memcpy(value, start, size);
            value[size] = '\0';
            return true;
        }
        ++p;
    }
    return false;
}

inline bool integer(const char *tag, const char *name, int64_t &value)
{
    char buffer[32], *end;
    if (!attribute(tag, name, buffer, sizeof(buffer)) || !buffer[0]) return false;
    errno = 0;
    value = std::strtoll(buffer, &end, 10);
    return *end == '\0' && errno != ERANGE;
}

inline bool number(const char *tag, const char *name, double &value)
{
    char buffer[48], *end;
    if (!attribute(tag, name, buffer, sizeof(buffer)) || !buffer[0]) return false;
    value = std::strtod(buffer, &end);
    return *end == '\0' && std::isfinite(value);
}

inline bool clip(double &x0, double &y0, double &x1, double &y1, int width, int height)
{
    const auto code = [=](double x, double y) {
        return (x < 0 ? 1 : x > width - 1 ? 2 : 0) | (y < 0 ? 4 : y > height - 1 ? 8 : 0);
    };
    for (int i = 0; i < 8; ++i) {
        const int a = code(x0, y0), b = code(x1, y1);
        if (!(a | b)) return true;
        if (a & b) return false;
        const int outside = a ? a : b;
        double x, y;
        if (outside & 12) {
            y = outside & 8 ? height - 1 : 0;
            x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        } else {
            x = outside & 2 ? width - 1 : 0;
            y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        }
        if (a) { x0 = x; y0 = y; }
        else { x1 = x; y1 = y; }
    }
    return false;
}

}
