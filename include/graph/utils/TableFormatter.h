/**
 * @file TableFormatter.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/1
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <iomanip>

namespace graph::storage {

class TableFormatter {
public:
    enum class BorderStyle {
        UNICODE,  // Uses Unicode box-drawing characters
        ASCII     // Uses simple ASCII characters (+, -, |)
    };

private:
    struct Column {
        std::string header;
        size_t width{};
        std::vector<std::string> values;
    };

    std::vector<Column> columns;
    BorderStyle style;
    std::string title;
    size_t padding;

    // Border characters based on style
    std::string topLeft, topRight, bottomLeft, bottomRight;
    std::string horizontal, vertical;
    std::string leftT, rightT, topT, bottomT, cross;

    void initBorderChars() {
        if (style == BorderStyle::UNICODE) {
            topLeft = "┌"; topRight = "┐"; bottomLeft = "└"; bottomRight = "┘";
            horizontal = "─"; vertical = "│";
            leftT = "├"; rightT = "┤"; topT = "┬"; bottomT = "┴"; cross = "┼";
        } else { // ASCII
            topLeft = "+"; topRight = "+"; bottomLeft = "+"; bottomRight = "+";
            horizontal = "-"; vertical = "|";
            leftT = "+"; rightT = "+"; topT = "+"; bottomT = "+"; cross = "+";
        }
    }

    [[nodiscard]] size_t getTableWidth() const {
        size_t width = 1; // Start with 1 for the left border
        for (const auto& col : columns) {
            width += col.width + 2 * padding + 1; // column width + padding + separator
        }
        return width;
    }

    void printHorizontalLine(std::ostream& os,
                            const std::string& left,
                            const std::string& mid,
                            const std::string& right) const {
        os << left;
        for (size_t i = 0; i < columns.size(); ++i) {
            os << std::string(columns[i].width + 2 * padding, horizontal[0]);
            if (i < columns.size() - 1) {
                os << mid;
            }
        }
        os << right << std::endl;
    }

public:
    explicit TableFormatter(BorderStyle style = BorderStyle::ASCII, size_t padding = 1)
        : style(style), padding(padding) {
        initBorderChars();
    }

    void setTitle(const std::string& title_) {
        this->title = title_;
    }

    void addColumn(const std::string& header) {
        Column col;
        col.header = header;
        col.width = header.length();
        columns.push_back(col);
    }

    void addRow(const std::vector<std::string>& rowData) {
        if (rowData.size() != columns.size()) {
            throw std::runtime_error("Row data size doesn't match column count");
        }

        for (size_t i = 0; i < columns.size(); ++i) {
            columns[i].values.push_back(rowData[i]);
            columns[i].width = std::max(columns[i].width, rowData[i].length());
        }
    }

    void print(std::ostream& os = std::cout) const {
        if (columns.empty()) return;

        // Calculate total width for title
        size_t tableWidth = getTableWidth();

        // Print top border
        printHorizontalLine(os, topLeft, topT, topRight);

        // Print title if present
        if (!title.empty()) {
            os << vertical << std::string(padding, ' ');

            size_t titleSpaceWidth = tableWidth - 2 - 2 * padding;
            size_t leftPadding = (titleSpaceWidth - title.length()) / 2;
            size_t rightPadding = titleSpaceWidth - title.length() - leftPadding;

            os << std::string(leftPadding, ' ') << title << std::string(rightPadding, ' ');
            os << std::string(padding, ' ') << vertical << std::endl;

            // Print separator after title
            printHorizontalLine(os, leftT, cross, rightT);
        }

        // Print headers
        os << vertical;
        for (const auto& col : columns) {
            os << std::string(padding, ' ')
               << std::left << std::setw(static_cast<int>(col.width)) << std::setfill(' ') << col.header
               << std::string(padding, ' ') << vertical;
        }
        os << std::endl;

        // Print separator after headers
        printHorizontalLine(os, leftT, cross, rightT);

        // Print data rows
        for (size_t row = 0; row < columns[0].values.size(); ++row) {
            os << vertical;
            for (const auto& col : columns) {
                os << std::string(padding, ' ')
                   << std::left << std::setw(static_cast<int>(col.width)) << std::setfill(' ') << col.values[row]
                   << std::string(padding, ' ') << vertical;
            }
            os << std::endl;
        }

        // Print bottom border
        printHorizontalLine(os, bottomLeft, bottomT, bottomRight);
    }

    void clear() {
        columns.clear();
        title.clear();
    }
};

} // namespace graph::storage