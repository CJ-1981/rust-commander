#pragma once

#include <QDateTime>
#include <QString>

// New names for many files at once, after Total Commander's multi-rename
// tool: a mask says what the new name is made of, and a find-and-replace and
// a case rule then work on what the mask made. Nothing here looks at the
// disk. A file is its name, its date and its place in the list; what comes
// out is its new name, or why it can't have one.
namespace RenameRules {

// What happens to the letters of the new name.
enum class Case {
    Unchanged,
    Lower,  // everything in lower case
    Upper,  // EVERYTHING IN UPPER CASE
    Title,  // The First Letter Of Each Word
};

// Everything that decides the new names. The rules are applied in this
// order: the mask, find and replace, underscores, and last the case.
struct Settings {
    // The new name out of literal text and placeholders in square brackets:
    //
    //   [N]      the name without its extension. A range takes part of it:
    //            [N2] the second character, [N2-5] the second to the fifth,
    //            [N2-] the second on, [N-5] the fifth from the end, [N-5-]
    //            the last five, [N2--5] the second to the fifth from the
    //            end, [N2,5] five from the second on. What lies outside the
    //            name is left out: [N2-5] of "ab" is "b". A letter with its
    //            accents is one character.
    //   [E]      the extension: what follows the last dot, unless that dot
    //            is the first or the last character of the name. A range as
    //            for [N].
    //   [C]      the counter: `counterStart` for the first file, and the
    //            step more for each file further down the list. [C10] starts
    //            at 10 instead, [C+5] five above the start, [C:3] is three
    //            digits wide ("007"); all of it at once, as in [C10+5:3].
    //   [Y] [M] [D] [h] [m] [s]
    //            year, month, day, hour, minute and second of the file's
    //            date: "2024", "03", "07", "09", "05", "31"
    //   [U] [L] [F] [n]
    //            from here on in the mask in upper case, in lower case, with
    //            the first letter of each word in upper case, or as it was
    //   [[] []]  the bracket [ and the bracket ]
    //
    // A dot right before an [E] that turns out empty is left out, so that
    // "[N].[E]" also fits a file without an extension.
    QString mask = "[N].[E]";

    // Where "find" is in what the mask made, "replace" comes instead, every
    // time it is there.
    QString find;
    QString replace;
    // `find` is a regular expression. Its groups can be used in `replace`:
    // $1, $2 and so on, ${name} for a named one, and $$ for a $. A literal
    // `find` matches in any case; a regular expression only in the case it
    // says, unless it starts with (?i).
    bool regularExpression = false;

    // The case of the new name, after everything else.
    Case caseMode = Case::Unchanged;

    int counterStart = 1;  // the counter of the first file
    int counterStep = 1;   // how much more each next file's counter is
    int counterWidth = 1;  // digits at least; what is missing comes in front as zeros

    // "_" becomes " ".
    bool underscoresToSpaces = false;
};

// A file, as far as the rules are concerned.
struct Source {
    QString name;        // with its extension
    QDateTime modified;  // the date the time placeholders use
    int index = 0;       // its place in the list; 0 is the first file
};

struct Result {
    // What the rules made of the name, also when it can't be used.
    QString name;
    // Why it can't be the file's name: what is wrong with the settings, an
    // empty name, or a name that FileOps::isAllowedName refuses. Empty when
    // all is well.
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// Why `settings` can't give any file a name, such as a placeholder that
// doesn't exist or a regular expression that isn't one. Empty when they can.
QString problem(const Settings &settings);

// The new name for `source`.
Result newName(const Source &source, const Settings &settings);

}  // namespace RenameRules
