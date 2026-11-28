#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// A line-by-line comparison of two texts, laid out in rows for a side-by-side
// view: what is the same, what is only on the left, only on the right, and
// what changed. Plain functions, with nothing that needs a window.
namespace LineDiff {

struct Options {
    // "a  " and "a" are the same line.
    bool ignoreTrailingWhitespace = true;
    // CRLF, LF and a lone CR all end a line alike.
    bool ignoreLineEndings = true;
    // Blank lines (empty, or only whitespace) are shown, but are no difference.
    bool ignoreBlankLines = false;
};

// A text cut into lines.
struct Text {
    QStringList lines;
    // Whether a line break follows the last line. A last line without one is
    // not the same as one with.
    bool endsWithLineBreak = true;
};

enum class Kind {
    Equal,    // the same line on both sides
    Removed,  // only on the left
    Added,    // only on the right
    Changed,  // the left line was replaced by the right one
    Ignored,  // a blank line that the options pass over: on one side only, but no difference
};

// Part of a line, counted in UTF-16 units from the start of the line.
struct Span {
    int start = 0;
    int length = 0;
};

struct Row {
    Kind kind = Kind::Equal;
    // Line numbers from 1; 0 where this side has nothing.
    int leftNumber = 0;
    int rightNumber = 0;
    QString left;   // empty when this side has nothing
    QString right;  // ... or when the line is empty
    // What differs inside a Changed row.
    QVector<Span> leftSpans;
    QVector<Span> rightSpans;
};

struct Result {
    // Every line of both texts, each in its own row on its own side.
    QVector<Row> rows;
    int removed = 0;  // lines only on the left; a changed pair counts on both sides
    int added = 0;    // lines only on the right
    int same = 0;     // lines that are the same
};

// What differs when `right` takes the place of `left`, for each side.
struct LineChanges {
    QVector<Span> left;
    QVector<Span> right;
};

// Cuts `text` into lines the way `options` says a line ends.
Text split(const QString &text, const Options &options = {});

// The parts of two lines that differ. Lines that are far apart are reported
// whole from their first to their last differing character.
LineChanges withinLine(const QString &left, const QString &right);

// Compares two texts line by line. It takes time proportional to the number
// of differing lines, not to the product of the line counts, and gives up on
// detail in regions that differ too much to be worth reading anyway.
Result compare(const Text &left, const Text &right, const Options &options = {});

}  // namespace LineDiff
