#include "RenameRules.h"

#include "ops/FileOps.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>
#include <QTextBoundaryFinder>

#include <optional>

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("RenameRules", text);
}

// Records why there is no name, and says so with the return value.
bool fail(QString *error, const QString &message)
{
    *error = message;
    return false;
}

bool isDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

// The number at `pos`, with a + or a - in front when it has one. `pos` moves
// past it; false when there is no number, or too many digits for an int.
bool readNumber(const QString &text, qsizetype *pos, int *value)
{
    qsizetype at = *pos;
    if (at < text.size() && (text.at(at) == QLatin1Char('+') || text.at(at) == QLatin1Char('-'))) {
        ++at;
    }
    const qsizetype digits = at;
    while (at < text.size() && isDigit(text.at(at))) {
        ++at;
    }
    if (at == digits) {
        return false;
    }
    bool ok = false;
    const int number = text.mid(*pos, at - *pos).toInt(&ok);
    if (!ok) {
        return false;
    }
    *pos = at;
    *value = number;
    return true;
}

// Characters of a name. Positions count from 1, or from the end of the name
// when they are negative.
struct Range {
    bool whole = true;  // no range given: all there is
    int from = 0;       // where it starts
    int to = 0;         // where it ends; 0 is the end of the name
    int count = 0;      // this many characters from `from` on, instead of up to `to`
};

// [C] with its numbers: start, step and digits, each when the placeholder says.
struct Counter {
    std::optional<int> start;
    std::optional<int> step;
    std::optional<int> width;
    // `start` is a distance from the settings' start, as in [C+5].
    bool relative = false;
};

// One part of the mask.
struct Token {
    enum class Kind {
        Text,       // as it stands
        Name,       // [N...]
        Extension,  // [E...]
        Counter,    // [C...]
        Date,       // [Y], [M], [D], [h], [m] or [s]
    };

    Kind kind = Kind::Text;
    // Text: the characters. Date: how QDateTime::toString() is asked for the part.
    QString text;
    Range range;      // Name, Extension
    Counter counter;  // Counter
    // What a [U], [L], [F] or [n] before it decided for it.
    RenameRules::Case mode = RenameRules::Case::Unchanged;
};

// A piece of the replacement text, or the group of the regular expression
// whose match goes in its place.
struct ReplacementPart {
    int group = -1;  // -1: `text`; 0: the whole match; 1, 2, ...: that group
    QString text;
};

// The settings looked into once. What is wrong with them for every file is in
// `problem`.
struct Rules {
    RenameRules::Settings settings;
    QList<Token> tokens;
    QRegularExpression expression{QString(), QRegularExpression::UseUnicodePropertiesOption};
    QList<ReplacementPart> replacement;
    QString problem;
};

// `placeholder` is what stands between the brackets, such as "N2-5".
bool parseRange(const QString &placeholder, Range *range, QString *error)
{
    const QString spec = placeholder.mid(1);
    if (spec.isEmpty()) {
        return true;
    }
    const auto wrong = [error, &placeholder] {
        return fail(error, tr("[%1] isn't a range of characters.").arg(placeholder));
    };

    Range parsed;
    parsed.whole = false;
    qsizetype pos = 0;
    // Position 0 doesn't exist: the first character is 1.
    if (!readNumber(spec, &pos, &parsed.from) || parsed.from == 0) {
        return wrong();
    }
    if (pos == spec.size()) {
        parsed.to = parsed.from;  // [N2]: the second character
    } else if (spec.at(pos) == QLatin1Char(',')) {
        ++pos;
        if (!readNumber(spec, &pos, &parsed.count) || parsed.count < 1) {
            return wrong();
        }
    } else if (spec.at(pos) == QLatin1Char('-')) {
        ++pos;
        // Nothing behind the dash is the end of the name: [N2-], [N-5-].
        if (pos < spec.size() && (!readNumber(spec, &pos, &parsed.to) || parsed.to == 0)) {
            return wrong();
        }
    } else {
        return wrong();
    }
    if (pos != spec.size()) {
        return wrong();
    }
    *range = parsed;
    return true;
}

bool parseCounter(const QString &placeholder, Counter *counter, QString *error)
{
    const QString spec = placeholder.mid(1);
    const auto wrong = [error, &placeholder] {
        return fail(error, tr("[%1] isn't a counter.").arg(placeholder));
    };

    Counter parsed;
    // A sign in front of the start makes it a distance from the settings' start.
    parsed.relative = !spec.isEmpty()
                      && (spec.at(0) == QLatin1Char('+') || spec.at(0) == QLatin1Char('-'));
    qsizetype pos = 0;
    int value = 0;
    if (readNumber(spec, &pos, &value)) {
        parsed.start = value;
    } else if (parsed.relative) {
        return wrong();
    }
    if (pos < spec.size() && (spec.at(pos) == QLatin1Char('+') || spec.at(pos) == QLatin1Char('-'))) {
        if (!readNumber(spec, &pos, &value)) {
            return wrong();
        }
        parsed.step = value;
    }
    if (pos < spec.size() && spec.at(pos) == QLatin1Char(':')) {
        ++pos;
        if (!readNumber(spec, &pos, &value) || value < 1) {
            return wrong();
        }
        parsed.width = value;
    }
    if (pos != spec.size()) {
        return wrong();
    }
    *counter = parsed;
    return true;
}

// `mask` as the tokens it is made of.
bool parseMask(const QString &mask, QList<Token> *tokens, QString *error)
{
    using Kind = Token::Kind;
    using RenameRules::Case;

    // The case that [U], [L], [F] and [n] last chose, for everything after it.
    Case mode = Case::Unchanged;
    QString text;
    // The literal text read so far is a token of its own, in the case that
    // went for it.
    const auto flush = [&] {
        if (!text.isEmpty()) {
            tokens->append(Token{.kind = Kind::Text, .text = text, .mode = mode});
            text.clear();
        }
    };

    for (qsizetype i = 0; i < mask.size(); ++i) {
        if (mask.at(i) != QLatin1Char('[')) {
            text += mask.at(i);
            continue;
        }
        const qsizetype end = mask.indexOf(QLatin1Char(']'), i + 1);
        if (end < 0) {
            return fail(error, tr("A \"[\" in the mask has no \"]\"."));
        }
        const QString placeholder = mask.mid(i + 1, end - i - 1);
        i = end;

        // "[[]" is a [; "[]]" is a ], from the empty placeholder and the ] after it.
        if (placeholder == QLatin1String("[")) {
            text += QLatin1Char('[');
            continue;
        }
        if (placeholder.isEmpty()) {
            continue;
        }

        Token token;
        token.mode = mode;
        const QChar kind = placeholder.at(0);
        const char *format = nullptr;
        if (kind == QLatin1Char('N') || kind == QLatin1Char('E')) {
            if (!parseRange(placeholder, &token.range, error)) {
                return false;
            }
            token.kind = kind == QLatin1Char('N') ? Kind::Name : Kind::Extension;
        } else if (kind == QLatin1Char('C')) {
            if (!parseCounter(placeholder, &token.counter, error)) {
                return false;
            }
            token.kind = Kind::Counter;
        } else if (placeholder.size() == 1) {
            switch (kind.unicode()) {
            case 'Y':
                format = "yyyy";
                break;
            case 'M':
                format = "MM";
                break;
            case 'D':
                format = "dd";
                break;
            case 'h':
                format = "hh";
                break;
            case 'm':
                format = "mm";
                break;
            case 's':
                format = "ss";
                break;
            default:
                // The rest of what a single letter can be: a case to go on in.
                Case next = mode;
                switch (kind.unicode()) {
                case 'U':
                    next = Case::Upper;
                    break;
                case 'L':
                    next = Case::Lower;
                    break;
                case 'F':
                    next = Case::Title;
                    break;
                case 'n':
                    next = Case::Unchanged;
                    break;
                default:
                    return fail(error, tr("Unknown placeholder [%1].").arg(placeholder));
                }
                flush();
                mode = next;
                continue;
            }
            token.kind = Kind::Date;
            token.text = QLatin1String(format);
        } else {
            return fail(error, tr("Unknown placeholder [%1].").arg(placeholder));
        }
        flush();
        tokens->append(token);
    }
    flush();
    return true;
}

// `replace` as it is used with `expression`. A group the expression doesn't
// have fails every file, so it is a problem of the settings.
bool parseReplacement(const QString &replace, const QRegularExpression &expression,
                      QList<ReplacementPart> *parts, QString *error)
{
    const QStringList names = expression.namedCaptureGroups();
    QString text;
    const auto flush = [&] {
        if (!text.isEmpty()) {
            parts->append(ReplacementPart{.group = -1, .text = text});
            text.clear();
        }
    };

    for (qsizetype i = 0; i < replace.size();) {
        if (replace.at(i) != QLatin1Char('$')) {
            text += replace.at(i);
            ++i;
            continue;
        }
        if (i + 1 >= replace.size() || replace.at(i + 1) == QLatin1Char('$')) {
            // A $ at the end, or $$: a $.
            text += QLatin1Char('$');
            i += i + 1 >= replace.size() ? 1 : 2;
            continue;
        }
        const QChar next = replace.at(i + 1);
        qsizetype pos = i + 1;
        QString name;
        if (isDigit(next)) {
            const qsizetype digits = pos;
            while (pos < replace.size() && isDigit(replace.at(pos))) {
                ++pos;
            }
            name = replace.mid(digits, pos - digits);
        } else if (next == QLatin1Char('{')) {
            const qsizetype end = replace.indexOf(QLatin1Char('}'), pos + 1);
            if (end < 0) {
                return fail(error, tr("A \"${\" in the replacement has no \"}\"."));
            }
            name = replace.mid(pos + 1, end - pos - 1);
            pos = end + 1;
        } else {
            text += QLatin1Char('$');
            ++i;
            continue;
        }

        // "$1" and "${1}" are a number; "${name}" is a name.
        bool number = !name.isEmpty();
        for (const QChar c : name) {
            number = number && isDigit(c);
        }
        int group = -1;
        if (number) {
            bool ok = false;
            group = name.toInt(&ok);
            if (!ok || group > expression.captureCount()) {
                group = -1;
            }
        } else {
            // An empty name would be found: it is the name of the nameless group, the whole match.
            group = name.isEmpty() ? -1 : int(names.indexOf(name));
        }
        if (group < 0) {
            return fail(error, tr("The replacement uses group %1, which the regular expression doesn't have.")
                                .arg(name));
        }
        flush();
        parts->append(ReplacementPart{.group = group});
        i = pos;
    }
    flush();
    return true;
}

Rules compile(const RenameRules::Settings &settings)
{
    Rules rules;
    rules.settings = settings;
    if (!parseMask(settings.mask, &rules.tokens, &rules.problem)) {
        return rules;
    }
    if (settings.counterWidth < 1) {
        rules.problem = tr("The counter needs at least one digit.");
        return rules;
    }
    if (!settings.regularExpression || settings.find.isEmpty()) {
        return rules;
    }
    rules.expression.setPattern(settings.find);
    if (!rules.expression.isValid()) {
        rules.problem = tr("The regular expression isn't valid: %1").arg(rules.expression.errorString());
        return rules;
    }
    parseReplacement(settings.replace, rules.expression, &rules.replacement, &rules.problem);
    return rules;
}

// `text` as the characters a reader sees: a Hangul syllable made of three
// jamo, or an emoji with its skin tone, is one.
QStringList graphemes(const QString &text)
{
    QStringList result;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    qsizetype from = 0;
    for (qsizetype to = finder.toNextBoundary(); to >= 0; to = finder.toNextBoundary()) {
        result.append(text.mid(from, to - from));
        from = to;
    }
    return result;
}

// The characters of a name that `range` names; `chars` is the name, split.
QString cut(const QStringList &chars, const Range &range)
{
    const int count = int(chars.size());
    const auto at = [count](int position) { return position > 0 ? position - 1 : count + position; };
    if (range.count > 0) {
        return chars.mid(qBound(0, at(range.from), count), range.count).join(QString());
    }
    const int first = qMax(0, at(range.from));
    const int last = range.to == 0 ? count : qMin(at(range.to) + 1, count);
    return first < last ? chars.mid(first, last - first).join(QString()) : QString();
}

// The counter of the file at `index`.
QString counterText(const Counter &counter, int index, const RenameRules::Settings &settings)
{
    const int start = counter.relative ? settings.counterStart + counter.start.value_or(0)
                                       : counter.start.value_or(settings.counterStart);
    const qint64 value = qint64(start) + qint64(index) * counter.step.value_or(settings.counterStep);
    QString digits = QString::number(value < 0 ? -value : value);
    digits = digits.rightJustified(qMax(1, counter.width.value_or(settings.counterWidth)), QLatin1Char('0'));
    return value < 0 ? QStringLiteral("-") + digits : digits;
}

// `text` with the first letter of every word in upper case; the rest stays as it is.
QString titled(const QString &text, bool startsWord)
{
    QString result = text;
    // At the start of a word, in front of its first letter: punctuation there,
    // such as a quote, is passed over; a number ends the beginning of the word.
    bool beginning = startsWord;
    for (qsizetype i = 0; i < result.size(); ++i) {
        const QChar c = result.at(i);
        if (c.isSpace()) {
            beginning = true;
        } else if (beginning && (c.isLetterOrNumber() || c.isMark())) {
            if (c.isLetter()) {
                result.replace(i, 1, QString(c).toUpper());
            }
            beginning = false;
        }
    }
    return result;
}

// `text` in the case `mode` asks for. `startsWord` says whether it begins a word.
QString inCase(const QString &text, RenameRules::Case mode, bool startsWord)
{
    switch (mode) {
    case RenameRules::Case::Lower:
        return text.toLower();
    case RenameRules::Case::Upper:
        return text.toUpper();
    case RenameRules::Case::Title:
        return titled(text, startsWord);
    default:
        return text;
    }
}

// What the mask makes of `source`'s name.
QString expand(const Rules &rules, const RenameRules::Source &source)
{
    // The extension is what follows the last dot, unless that dot is the
    // first or the last character of the name: ".profile" and "name." have none.
    const qsizetype lastDot = source.name.lastIndexOf(QLatin1Char('.'));
    const bool hasExtension = lastDot > 0 && lastDot < source.name.size() - 1;
    const QString name = hasExtension ? source.name.left(lastDot) : source.name;
    const QString extension = hasExtension ? source.name.mid(lastDot + 1) : QString();
    const QDateTime date = source.modified.isValid() ? source.modified : QDateTime::fromSecsSinceEpoch(0);

    // Split into characters only when a range asks for it.
    QStringList nameCharacters;
    QStringList extensionCharacters;
    bool charactersKnown = false;
    const auto characters = [&](QStringList *cache) -> const QStringList & {
        if (!charactersKnown) {
            nameCharacters = graphemes(name);
            extensionCharacters = graphemes(extension);
            charactersKnown = true;
        }
        return *cache;
    };

    QString result;
    // Whether what comes next begins a word, for [F].
    bool startsWord = true;
    const auto put = [&](const QString &piece, RenameRules::Case mode) {
        if (!piece.isEmpty()) {
            result += inCase(piece, mode, startsWord);
        }
        startsWord = result.isEmpty() || result.at(result.size() - 1).isSpace();
    };

    const QList<Token> &tokens = rules.tokens;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        const Token &token = tokens.at(i);
        QString piece;
        switch (token.kind) {
        case Token::Kind::Text:
            piece = token.text;
            break;
        case Token::Kind::Name:
            piece = token.range.whole ? name : cut(characters(&nameCharacters), token.range);
            break;
        case Token::Kind::Extension:
            piece = token.range.whole ? extension : cut(characters(&extensionCharacters), token.range);
            break;
        case Token::Kind::Counter:
            piece = counterText(token.counter, source.index, rules.settings);
            break;
        case Token::Kind::Date:
            piece = date.toString(token.text);
            break;
        }
        if (piece.isEmpty() && token.kind == Token::Kind::Extension && i > 0
            && tokens.at(i - 1).kind == Token::Kind::Text && result.endsWith(QLatin1Char('.'))) {
            // "[N].[E]" of a file without an extension is its name, not "name.".
            result.chop(1);
        }
        put(piece, token.mode);
    }
    return result;
}

// `text` with every "find" replaced, as the settings say.
QString replaceAll(const Rules &rules, const QString &text)
{
    const RenameRules::Settings &settings = rules.settings;
    if (settings.find.isEmpty()) {
        return text;
    }
    if (!settings.regularExpression) {
        QString result = text;
        // Behind what was put in, so that it isn't searched again.
        qsizetype from = 0;
        for (qsizetype at = result.indexOf(settings.find, from, Qt::CaseInsensitive); at >= 0;
             at = result.indexOf(settings.find, from, Qt::CaseInsensitive)) {
            result.replace(at, settings.find.size(), settings.replace);
            from = at + settings.replace.size();
        }
        return result;
    }

    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator matches = rules.expression.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += text.mid(last, match.capturedStart() - last);
        for (const ReplacementPart &part : rules.replacement) {
            result += part.group < 0 ? part.text : match.captured(part.group);
        }
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

RenameRules::Result run(const Rules &rules, const RenameRules::Source &source)
{
    if (!rules.problem.isEmpty()) {
        return {QString(), rules.problem};
    }
    QString name = replaceAll(rules, expand(rules, source));
    if (rules.settings.underscoresToSpaces) {
        name.replace(QLatin1Char('_'), QLatin1Char(' '));
    }
    name = inCase(name, rules.settings.caseMode, true);

    if (name.isEmpty()) {
        return {name, tr("The new name would be empty.")};
    }
    // What a file is called already is left at that, allowed or not: it isn't renamed.
    QString reason;
    if (name != source.name && !FileOps::isAllowedName(name, &reason)) {
        return {name, reason};
    }
    return {name, QString()};
}

}  // namespace

namespace RenameRules {

QString problem(const Settings &settings)
{
    return compile(settings).problem;
}

Result newName(const Source &source, const Settings &settings)
{
    return run(compile(settings), source);
}

}  // namespace RenameRules
