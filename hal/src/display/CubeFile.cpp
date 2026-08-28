#include "CubeFile.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace Gaokun::Display {

namespace {

[[nodiscard]] std::vector<std::string> Tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

[[nodiscard]] bool ParseFloat(const std::string &text, float &out) noexcept {
    try {
        size_t consumed = 0;
        out = std::stof(text, &consumed);
        return consumed == text.size();
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool ParseSize(const std::string &text, int &out) noexcept {
    const auto *last = text.data() + text.size();
    const auto result = std::from_chars(text.data(), last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] std::string At(int line, const std::string &message) {
    return "line " + std::to_string(line) + ": " + message;
}

} // namespace

bool ReadCubeFile(const std::wstring &path, CubeFile &out, std::string &error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open the file";
        return false;
    }

    int size = 0;
    int dimension = 0;
    std::vector<Rgb> rows;
    int lineNumber = 0;
    std::string line;

    while (std::getline(file, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const auto tokens = Tokenize(line);
        if (tokens.empty() || tokens[0][0] == '#') continue;

        if (tokens[0] == "TITLE") continue;

        if (tokens[0] == "DOMAIN_MIN" || tokens[0] == "DOMAIN_MAX") {
            if (tokens.size() != 4) {
                error = At(lineNumber, "malformed " + tokens[0]);
                return false;
            }
            // 只支持 0..1 的定义域。硬件表本身就按 0..1 归一，非默认定义域需要在采样前
            // 做一次线性映射，而带这种头的 .cube 极少见，与其悄悄按 0..1 处理不如直接拒绝。
            for (size_t i = 1; i < 4; ++i) {
                float value = 0.0f;
                if (!ParseFloat(tokens[i], value)) {
                    error = At(lineNumber, "malformed " + tokens[0]);
                    return false;
                }
                const float expected = tokens[0] == "DOMAIN_MIN" ? 0.0f : 1.0f;
                if (value != expected) {
                    error = At(lineNumber, tokens[0] + " other than the default is not supported");
                    return false;
                }
            }
            continue;
        }

        if (tokens[0] == "LUT_3D_SIZE" || tokens[0] == "LUT_1D_SIZE") {
            if (dimension != 0) {
                error = At(lineNumber, "duplicate table size");
                return false;
            }
            if (tokens.size() != 2 || !ParseSize(tokens[1], size)) {
                error = At(lineNumber, "malformed " + tokens[0]);
                return false;
            }
            dimension = tokens[0] == "LUT_3D_SIZE" ? 3 : 1;
            const int maximum = dimension == 3 ? 256 : 65536;
            if (size < 2 || size > maximum) {
                error = At(lineNumber, "unsupported table size " + std::to_string(size));
                return false;
            }
            rows.reserve(dimension == 3 ? static_cast<size_t>(size) * size * size
                                        : static_cast<size_t>(size));
            continue;
        }

        if (dimension == 0) {
            error = At(lineNumber, "table data appeared before the table size");
            return false;
        }
        if (tokens.size() != 3) {
            error = At(lineNumber, "malformed table row");
            return false;
        }

        Rgb entry{};
        if (!ParseFloat(tokens[0], entry.r) || !ParseFloat(tokens[1], entry.g) ||
            !ParseFloat(tokens[2], entry.b)) {
            error = At(lineNumber, "malformed table row");
            return false;
        }
        rows.push_back(entry);
    }

    if (dimension == 0) {
        error = "no LUT_3D_SIZE or LUT_1D_SIZE found";
        return false;
    }

    const size_t expected = dimension == 3
                                ? static_cast<size_t>(size) * size * size
                                : static_cast<size_t>(size);
    if (rows.size() != expected) {
        error = "expected " + std::to_string(expected) + " entries, found " +
                std::to_string(rows.size());
        return false;
    }

    if (dimension == 1) {
        out.lut1d = Lut1d(size);
        for (int i = 0; i < size; ++i) out.lut1d.At(i) = rows[static_cast<size_t>(i)];
        return true;
    }

    // .cube 的行序是 red 变化最快，与 Lut3d 内部索引一致，逐行填入即可。
    out.lut3d = Lut3d(size);
    size_t index = 0;
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                out.lut3d.At(r, g, b) = rows[index++];
            }
        }
    }
    return true;
}

} // namespace Gaokun::Display
