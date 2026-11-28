#include "FileSearch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace FileSearch {

namespace {

constexpr int kMaxDepth = 64;                   // as in FileOps: a guard against anything that leads round in circles
constexpr qsizetype kSniffBytes = 8192;         // this much is read to tell text from the rest
constexpr qsizetype kMaxLineText = 240;         // how much of a line is kept
constexpr qint64 kReadChunk = 2 * 1024 * 1024;  // files are looked into this much at a time
constexpr qint64 kReadOverlap = 4096;           // consecutive chunks share this much
constexpr qint64 kProgressEvery = 256;          // entries between looks at `stop` and progress reports

QString tr(const char *text)
{
    return QCoreApplication::translate("FileSearch", text);
}

// Paths differ in case only on Windows.
#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

fs::path toPath(const QString &text)
{
#ifdef Q_OS_WIN
    return fs::path(text.toStdWString());
#else
    return fs::path(text.toStdString());
#endif
}

// Paths are kept with forward slashes everywhere, as Qt keeps them.
QString toQString(const fs::path &path)
{
#ifdef Q_OS_WIN
    QString text = QString::fromStdWString(path.native());
    text.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return text;
#else
    return QString::fromStdString(path.native());
#endif
}

// Whether `path` is `folder` itself or lies inside it.
bool isAtOrInside(const QString &path, const QString &folder)
{
    return path.compare(folder, kPathCase) == 0
           || path.startsWith(folder + QLatin1Char('/'), kPathCase);
}

// Unix hides names that start with a dot; Windows goes by an attribute.
bool isHidden(const QString &name, const QString &path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(name)
    return QFileInfo(path).isHidden();
#else
    Q_UNUSED(path)
    return name.startsWith(QLatin1Char('.'));
#endif
}

// The pattern compiled. `error` says what is wrong with it, the first thing
// that is; an invalid pattern matches nothing.
QRegularExpression compile(const QString &pattern, Qt::CaseSensitivity cs,
                          QRegularExpression::PatternOptions options, QString *error)
{
    const QRegularExpression::PatternOptions all =
            options | (cs == Qt::CaseSensitive ? QRegularExpression::NoPatternOption
                                               : QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression expression(pattern, all);
    if (!expression.isValid() && error && error->isEmpty()) {
        *error = tr("\"%1\" is not a regular expression: %2").arg(pattern, expression.errorString());
    }
    return expression;
}

// A query made ready to be asked about many names at once: the patterns are
// compiled here, once, and everything that is not about a name is kept here.
// An invalid pattern leaves `error` set; the matcher then matches nothing.
class Matcher {
public:
    explicit Matcher(const Query &query)
        : m_what(query.what)
        , m_includeHidden(query.includeHidden)
        , m_minSize(query.minSize)
        , m_maxSize(query.maxSize)
        , m_after(query.modifiedAfter)
        , m_before(query.modifiedBefore)
    {
        const Qt::CaseSensitivity cs = query.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
        // Typed text comes composed, but names on disk may be decomposed
        // (macOS keeps them as they were created), so patterns and names are
        // both brought into composed form before they meet.
        if (query.nameIsRegex) {
            m_names.append(compile(query.name.normalized(QString::NormalizationForm_C), cs,
                                   QRegularExpression::UseUnicodePropertiesOption, &m_error));
        } else {
            const QStringList words =
                    query.name.normalized(QString::NormalizationForm_C).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString &word : words) {
                // A plain word may be anywhere in the name, and fromWildcard()
                // matches whole names only, so it is wrapped in stars. A word
                // with a wildcard in it is the whole name instead: "*.jpg"
                // finds jpg files, not "photo.jpg.bak".
                const bool glob = word.contains(QLatin1Char('*')) || word.contains(QLatin1Char('?'));
                const QString pattern = glob ? word : QLatin1Char('*') + word + QLatin1Char('*');
                m_names.append(QRegularExpression::fromWildcard(pattern, cs));
            }
        }
    }

    const QString &error() const { return m_error; }

    // Whether files or folders are looked for at all.
    bool looksIn(bool isDir) const
    {
        return m_what == Query::What::Both || (m_what == Query::What::Folders) == isDir;
    }

    // Whether a hidden entry gets through.
    bool wants(bool hidden) const { return !hidden || m_includeHidden; }

    // Size and date. A folder has no size here, so a size range leaves every
    // folder out, and what has no date can't be known to be in a date range.
    bool fits(bool isDir, qint64 size, const QDateTime &modified) const
    {
        if (m_minSize >= 0 || m_maxSize >= 0) {
            if (isDir || size < 0 || (m_minSize >= 0 && size < m_minSize)
                || (m_maxSize >= 0 && size > m_maxSize)) {
                return false;
            }
        }
        if (!modified.isValid()) {
            return !m_after.isValid() && !m_before.isValid();
        }
        return (!m_after.isValid() || modified >= m_after)
               && (!m_before.isValid() || modified <= m_before);
    }

    // Where the name pattern matched `name`, which must be composed. False
    // when not every word was found, and `spans` is emptied then.
    bool nameMatches(const QString &name, QList<Span> *spans) const
    {
        for (const QRegularExpression &pattern : m_names) {
            const QRegularExpressionMatch found = pattern.match(name);
            if (!found.hasMatch()) {
                spans->clear();
                return false;
            }
            // What matched nothing (an empty pattern) is nothing to show.
            if (found.capturedLength() > 0) {
                spans->append(Span{int(found.capturedStart()), int(found.capturedEnd())});
            }
        }
        return true;
    }

private:
    Query::What m_what;
    bool m_includeHidden;
    qint64 m_minSize;
    qint64 m_maxSize;
    QDateTime m_after;
    QDateTime m_before;

    QList<QRegularExpression> m_names;
    QString m_error;
};

// Looking inside files for text.
struct TextSearch {
    QString needle;  // the plain text looked for; empty when `pattern` is used
    QRegularExpression pattern;
    Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
    qint64 overlap = kReadOverlap;  // what two chunks of a file share
    bool isRegex = false;
};

TextSearch textSearchFor(const Query::Content &content, Qt::CaseSensitivity cs, QString *error)
{
    TextSearch search;
    search.caseSensitivity = cs;
    search.isRegex = content.isRegex;
    if (content.isRegex) {
        // In a file, ^ and $ are the ends of a line, as they are for grep.
        search.pattern = compile(content.text, cs,
                                 QRegularExpression::UseUnicodePropertiesOption
                                         | QRegularExpression::MultilineOption,
                                 error);
    } else {
        search.needle = content.text;
        // Text longer than the overlap could be cut in two by it.
        search.overlap = qMax(kReadOverlap, qint64(content.text.toUtf8().size()));
    }
    return search;
}

// Where the text is in `text`; -1 when it isn't there.
int findText(const TextSearch &search, const QString &text)
{
    if (search.isRegex) {
        const QRegularExpressionMatch found = search.pattern.match(text);
        return found.hasMatch() ? int(found.capturedStart()) : -1;
    }
    return int(text.indexOf(search.needle, 0, search.caseSensitivity));
}

// Whether text has anything in it that composition would change: combining
// marks, and the letters Korean is built from when it is decomposed. Plain
// ASCII and composed text go through quickly.
bool needsComposing(const QString &text)
{
    for (const QChar c : text) {
        const ushort u = c.unicode();
        if (u >= 0x0300
            && (QChar::combiningClass(u) != 0 || (u >= 0x1100 && u <= 0x11ff) || (u >= 0xd7b0 && u <= 0xd7ff))) {
            return true;
        }
    }
    return false;
}

// The line the text was found on, as far as there is room for: a long line is
// shown around where the match is.
QString lineOf(const QString &text, qsizetype lineStart, qsizetype lineEnd, qsizetype at)
{
    qsizetype from = lineStart;
    qsizetype to = lineEnd;
    if (to - from > kMaxLineText) {
        from = qMax(lineStart, at - kMaxLineText / 2);
        to = qMin(lineEnd, from + kMaxLineText);
        from = qMax(lineStart, to - kMaxLineText);
    }
    QString line = text.mid(from, to - from).trimmed();
    if (from > lineStart) {
        line.prepend(QChar(0x2026));  // something came before
    }
    if (to < lineEnd) {
        line.append(QChar(0x2026));
    }
    return line;
}

// Whether the file has the text in it. Files are read in chunks that overlap,
// so that a big file costs little memory while text is never cut in two (a
// regular expression, though, has to match inside one chunk). The first match
// is reported with the line it is on. `stopped` is set when `stop` ended the
// reading before the file was through.
bool hasText(const QString &path, const TextSearch &search, const std::atomic<bool> &stop, int *line,
             QString *lineText, bool *stopped)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    qint64 start = 0;        // where in the file the chunk being read begins
    qint64 linesBefore = 0;  // line breaks before that
    bool first = true;
    while (true) {
        QByteArray bytes;
        while (bytes.size() < kReadChunk && !file.atEnd()) {
            const QByteArray part = file.read(kReadChunk - bytes.size());
            if (part.isEmpty()) {
                break;
            }
            bytes += part;
        }
        if (bytes.isEmpty()) {
            break;
        }
        // One zero byte near the start says: not text, whatever it is called.
        if (first && bytes.left(kSniffBytes).contains('\0')) {
            break;
        }

        QString text = QString::fromUtf8(bytes);
        int at = findText(search, text);
        if (at < 0 && !search.isRegex && needsComposing(text)) {
            // Some programs save Korean and other text decomposed, character
            // by character, while typed text comes composed: the same text
            // spelled two ways never meets unless one of them is composed.
            text = text.normalized(QString::NormalizationForm_C);
            at = findText(search, text);
        }
        if (at >= 0) {
            const qsizetype lineStart = at > 0 ? text.lastIndexOf(QLatin1Char('\n'), at - 1) + 1 : 0;
            qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), at);
            if (lineEnd < 0) {
                lineEnd = text.size();
            }
            *line = int(linesBefore + std::count(text.cbegin(), text.cbegin() + at, QChar(u'\n')) + 1);
            *lineText = lineOf(text, lineStart, lineEnd, at);
            return true;
        }
        if (file.atEnd()) {
            break;
        }

        // The next chunk starts before this one ends, and never in the middle
        // of a character.
        qsizetype cut = bytes.size() - search.overlap;
        while (cut > 0 && (uchar(bytes.at(cut)) & 0xc0) == 0x80) {
            --cut;
        }
        if (cut <= 0 || !file.seek(start + cut)) {
            break;
        }
        linesBefore += std::count(bytes.cbegin(), bytes.cbegin() + cut, '\n');
        start += cut;
        first = false;
        if (stop.load(std::memory_order_relaxed)) {
            *stopped = true;
            break;
        }
    }
    return false;
}

// One folder still to look into, and how deep it lies below a folder the
// search started from.
struct Place {
    fs::path path;
    int depth = 0;
};

// Does the looking, and fills `summary`, also when it returns early.
void walk(const Query &query, const Matcher &matcher, const TextSearch &text, const Callback &callback,
          std::atomic<bool> &stop, Summary *summary)
{
    // Ends the walk when `stop` is set.
    const auto giveUp = [&] {
        if (!stop.load(std::memory_order_relaxed)) {
            return false;
        }
        summary->stopped = true;
        return true;
    };

    // Breadth first, so that what lies near the top is found first. A start
    // folder inside another one is left out, so that nothing is found twice.
    QStringList roots;
    std::deque<Place> queue;
    for (const QString &folder : query.folders) {
        const QString path = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
        const auto in = [&](const QString &inner, const QString &outer) {
            return isAtOrInside(inner, outer);
        };
        if (std::any_of(roots.begin(), roots.end(), [&](const QString &root) { return in(path, root); })) {
            continue;
        }
        roots.erase(std::remove_if(roots.begin(), roots.end(),
                                   [&](const QString &root) { return in(root, path); }),
                    roots.end());
        roots.append(path);
    }
    for (const QString &root : roots) {
        queue.push_back(Place{toPath(root), 0});
    }

    Progress progress;
    qint64 found = 0;
    while (!queue.empty()) {
        const Place place = queue.front();
        queue.pop_front();
        if (giveUp()) {
            return;
        }

        std::error_code ec;
        fs::directory_iterator it(place.path, ec);
        if (ec) {
            // Not ours to read, or gone already.
            ++summary->unreadable;
            continue;
        }

        ++summary->folders;
        progress.folders = summary->folders;
        progress.current = toQString(place.path);
        callback(nullptr, progress);

        for (; it != fs::directory_iterator(); it.increment(ec)) {
            const fs::directory_entry &entry = *it;
            ++summary->entries;
            if (summary->entries % kProgressEvery == 0) {
                if (giveUp()) {
                    return;
                }
                progress.entries = summary->entries;
                callback(nullptr, progress);
            }

            const QString path = toQString(entry.path());
            const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
            // A link to a folder counts as a folder, but is not gone into:
            // links can lead back to a folder that was looked at already.
            const bool isLink = entry.is_symlink(ec);
            const bool isDir = entry.is_directory(ec);
            const bool hidden = isHidden(name, path);
            if (!matcher.wants(hidden)) {
                continue;
            }
            if (isDir && !isLink && query.recursive && place.depth < kMaxDepth) {
                queue.push_back(Place{entry.path(), place.depth + 1});
            }
            // With text to find, there is nothing to look for in a folder.
            if (!matcher.looksIn(isDir) || (isDir && query.content)) {
                continue;
            }

            QList<Span> spans;
            const QString composed = name.normalized(QString::NormalizationForm_C);
            if (!matcher.nameMatches(composed, &spans)) {
                continue;
            }

            // Size and date are read for what may be shown: one stat for each
            // of those, and none for the rest.
            const QFileInfo info(path);
            const qint64 size = isDir ? -1 : info.size();
            const QDateTime modified = info.lastModified();
            if (!matcher.fits(isDir, size, modified)) {
                continue;
            }

            Match match;
            match.path = path;
            match.folder = path.left(path.size() - name.size() - 1);
            match.name = composed;
            match.size = size;
            match.modified = modified;
            match.isDir = isDir;
            match.hidden = hidden;
            match.spans = spans;
            if (query.content) {
                if (query.content->maxSize >= 0 && size > query.content->maxSize) {
                    continue;
                }
                bool stopped = false;
                if (!hasText(path, text, stop, &match.line, &match.lineText, &stopped)) {
                    if (stopped) {
                        summary->stopped = true;
                        return;
                    }
                    continue;
                }
            }

            callback(&match, progress);
            if (giveUp()) {
                return;
            }
            if (query.maxResults > 0 && ++found >= query.maxResults) {
                summary->capped = true;
                return;
            }
        }
    }
}

}  // namespace

QString validate(const Query &query)
{
    const Matcher matcher(query);
    if (!matcher.error().isEmpty()) {
        return matcher.error();
    }
    if (query.folders.isEmpty()) {
        return tr("There is no folder to look in.");
    }
    for (const QString &folder : query.folders) {
        const QFileInfo info(folder);
        if (!info.exists() || !info.isDir()) {
            return tr("This is not a folder, or it no longer exists: %1").arg(folder);
        }
    }
    if (query.what == Query::What::Folders && query.content) {
        return tr("Folders have no text in them: look for files, or for both, to search inside them.");
    }
    if (query.minSize >= 0 && query.maxSize >= 0 && query.minSize > query.maxSize) {
        return tr("No file is between those two sizes: the smaller one has to come first.");
    }
    if (query.modifiedAfter.isValid() && query.modifiedBefore.isValid()
        && query.modifiedAfter > query.modifiedBefore) {
        return tr("Nothing was changed between those two dates: the earlier one has to come first.");
    }
    return {};
}

bool matches(const Query &query, const QString &name, bool isDir, qint64 size, const QDateTime &modified,
             bool hidden)
{
    const Matcher matcher(query);
    if (!matcher.error().isEmpty() || !matcher.wants(hidden) || !matcher.looksIn(isDir)) {
        return false;
    }
    // Names are matched in composed form, whichever form they came in.
    QList<Span> spans;
    return matcher.fits(isDir, size, modified)
           && matcher.nameMatches(name.normalized(QString::NormalizationForm_C), &spans);
}

Summary search(const Query &query, Callback callback, std::atomic<bool> &stop)
{
    Summary summary;
    summary.error = validate(query);
    if (!summary.error.isEmpty()) {
        return summary;
    }
    // The patterns are valid now, as validate() has seen.
    const Matcher matcher(query);
    const Qt::CaseSensitivity cs = query.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const TextSearch text = textSearchFor(query.content.value_or(Query::Content{}), cs, nullptr);
    const Callback report = callback ? callback : [](const Match *, const Progress &) {};
    walk(query, matcher, text, report, stop, &summary);
    return summary;
}

}  // namespace FileSearch
