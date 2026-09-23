#pragma once

#include <string>
#include <string_view>
#include <vector>

struct AnsiScreenCell {
    char text = ' ';
    std::string foreground;
    std::string background;
    std::string underline;

    bool operator==(const AnsiScreenCell&) const = default;
};

class AnsiScreenModel {
  public:
    AnsiScreenModel(int columns, int rows)
        : columns_{columns}, rows_{rows},
          cells_(static_cast<std::size_t>(columns * rows)) {}

    void apply(std::string_view bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            if (bytes[offset] != '\x1b') {
                if (row_ >= 0 && row_ < rows_ && column_ >= 0 &&
                    column_ < columns_) {
                    auto& cell = cells_[static_cast<std::size_t>(
                        row_ * columns_ + column_)];
                    cell = {bytes[offset], foreground_, background_,
                            underline_};
                }
                ++column_;
                ++offset;
                continue;
            }
            if (offset + 1 >= bytes.size()) break;
            if (bytes[offset + 1] == ']') {
                auto end = bytes.find("\x1b\\", offset + 2);
                if (end == std::string_view::npos) break;
                offset = end + 2;
                continue;
            }
            if (bytes[offset + 1] != '[') {
                offset += 2;
                continue;
            }
            auto end = offset + 2;
            while (end < bytes.size() &&
                   !((bytes[end] >= '@' && bytes[end] <= '~'))) {
                ++end;
            }
            if (end >= bytes.size()) break;
            const auto body =
                bytes.substr(offset + 2, end - offset - 2);
            const char final = bytes[end];
            if (final == 'H') {
                auto separator = body.find(';');
                row_ = parse(body.substr(0, separator)) - 1;
                column_ =
                    separator == std::string_view::npos
                        ? 0
                        : parse(body.substr(separator + 1)) - 1;
            } else if (final == 'm') {
                applySgr(body);
            } else if ((final == 'h' || final == 'l') &&
                       body == "?25") {
                cursorVisible_ = final == 'h';
            }
            offset = end + 1;
        }
    }

    bool operator==(const AnsiScreenModel&) const = default;

  private:
    static int parse(std::string_view digits) {
        int value = 0;
        for (char digit : digits) {
            if (digit < '0' || digit > '9') break;
            value = value * 10 + digit - '0';
        }
        return value == 0 ? 1 : value;
    }

    void applySgr(std::string_view body) {
        if (body == "0") {
            foreground_.clear();
            background_.clear();
            underline_.clear();
        } else if (body.starts_with("38;") ||
                   (body.size() == 2 &&
                    ((body[0] == '3') || (body[0] == '9')))) {
            foreground_ = std::string{body};
        } else if (body.starts_with("48;") ||
                   (body.size() == 2 &&
                    ((body[0] == '4') || (body[0] == '1')))) {
            background_ = std::string{body};
        } else if (body == "4:0") {
            underline_.clear();
        } else if (body.starts_with("4:") || body.starts_with("58;")) {
            underline_ += std::string{body} + ";";
        }
    }

    int columns_;
    int rows_;
    std::vector<AnsiScreenCell> cells_;
    int column_ = 0;
    int row_ = 0;
    bool cursorVisible_ = true;
    std::string foreground_;
    std::string background_;
    std::string underline_;
};
