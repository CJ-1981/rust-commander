#include "Checksums.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <array>

namespace {

constexpr qint64 kChunkBytes = 1 << 20;         // this much of a file is read at a time
constexpr qint64 kMaxListBytes = 32LL << 20;    // no list of checksums is this big
constexpr qint64 kLookInsideBytes = 64 * 1024;  // this much of a file is read to tell a checksum file by its content

// What a tool printed and someone kept: the algorithm, the file in brackets, the hash.
const QRegularExpression kTaggedLine(R"(^(\w[\w.-]*)\s*\((.*)\)\s*=\s*([0-9A-Fa-f]+)$)");
const QRegularExpression kSpace("\\s");
const QRegularExpression kHexRun("[0-9A-Fa-f]+");

QString tr(const char *text)
{
    return QCoreApplication::translate("Checksums", text);
}

bool isHexDigit(char16_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isHex(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar c : text) {
        if (!isHexDigit(c.unicode())) {
            return false;
        }
    }
    return true;
}

// The table of the CRC-32 that zlib and SFV files use: this polynomial,
// reflected, with the value inverted before and after.
constexpr std::array<quint32, 256> crcTable()
{
    std::array<quint32, 256> table{};
    for (quint32 byte = 0; byte < 256; ++byte) {
        quint32 value = byte;
        for (int bit = 0; bit < 8; ++bit) {
            value = value & 1u ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        }
        table[byte] = value;
    }
    return table;
}

constexpr std::array<quint32, 256> kCrcTable = crcTable();

// A hash while it is computed. CRC32 is done here, as Qt has no such algorithm.
class Hasher {
public:
    explicit Hasher(Checksums::Algorithm algorithm)
    {
        switch (algorithm) {
        case Checksums::Algorithm::Md5:
            m_qt.emplace(QCryptographicHash::Md5);
            break;
        case Checksums::Algorithm::Sha1:
            m_qt.emplace(QCryptographicHash::Sha1);
            break;
        case Checksums::Algorithm::Sha256:
            m_qt.emplace(QCryptographicHash::Sha256);
            break;
        case Checksums::Algorithm::Sha512:
            m_qt.emplace(QCryptographicHash::Sha512);
            break;
        case Checksums::Algorithm::Crc32:
            break;
        }
    }

    void add(const char *data, qsizetype size)
    {
        if (m_qt) {
            m_qt->addData(QByteArrayView(data, size));
            return;
        }
        for (qsizetype i = 0; i < size; ++i) {
            m_crc = kCrcTable[(m_crc ^ static_cast<unsigned char>(data[i])) & 0xFFu] ^ (m_crc >> 8);
        }
    }

    // In lower case.
    QString hex() const
    {
        if (m_qt) {
            return QString::fromLatin1(m_qt->result().toHex());
        }
        return QString::number(m_crc ^ 0xFFFFFFFFu, 16).rightJustified(8, QLatin1Char('0'));
    }

private:
    std::optional<QCryptographicHash> m_qt;
    quint32 m_crc = 0xFFFFFFFFu;
};

// UTF-8 if it is that; else what the system uses for files, as a checksum file
// written on Windows may hold names in its code page.
QString decode(const QByteArray &bytes)
{
    const QString utf8 = QString::fromUtf8(bytes);
    if (!utf8.contains(QChar::ReplacementCharacter)) {
        return utf8;
    }
    return QString::fromLocal8Bit(bytes);
}

// The algorithm of a hash written without a name, told by how long it is.
std::optional<Checksums::Algorithm> algorithmForHex(const QString &text)
{
    return isHex(text) ? Checksums::algorithmForLength(int(text.size())) : std::nullopt;
}

Checksums::Entry entry(const QString &name, const QString &hash, Checksums::Algorithm algorithm)
{
    // As the file is read, with a leading "./" left out.
    return Checksums::Entry{QDir::cleanPath(name), hash, algorithm};
}

// The reading of one file of a check, counted from what was read before it.
class Counting : public Checksums::Progress {
public:
    Counting(Checksums::Progress *all, qint64 before, qint64 total)
        : m_all(all)
        , m_before(before)
        , m_total(total)
    {
    }

    bool bytes(qint64 done, qint64) override
    {
        return m_all->bytes(m_before + done, m_total);
    }

private:
    Checksums::Progress *m_all;
    qint64 m_before;
    qint64 m_total;
};

// Where the file of an entry is: in `folder`, under its name. A name written
// on Windows, with backslashes, is looked for with slashes too.
QString filePathIn(const QString &folder, const QString &name)
{
    const QDir dir(folder);
    const QString path = dir.absoluteFilePath(name);
    if (!QFileInfo::exists(path) && name.contains(QLatin1Char('\\'))) {
        return dir.absoluteFilePath(QString(name).replace(QLatin1Char('\\'), QLatin1Char('/')));
    }
    return path;
}

}  // namespace

namespace Checksums {

QString algorithmName(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::Crc32:
        return QStringLiteral("CRC32");
    case Algorithm::Md5:
        return QStringLiteral("MD5");
    case Algorithm::Sha1:
        return QStringLiteral("SHA-1");
    case Algorithm::Sha256:
        return QStringLiteral("SHA-256");
    case Algorithm::Sha512:
        return QStringLiteral("SHA-512");
    }
    return QString();
}

QString fileSuffix(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::Crc32:
        return QStringLiteral("sfv");
    case Algorithm::Md5:
        return QStringLiteral("md5");
    case Algorithm::Sha1:
        return QStringLiteral("sha1");
    case Algorithm::Sha256:
        return QStringLiteral("sha256");
    case Algorithm::Sha512:
        return QStringLiteral("sha512");
    }
    return QString();
}

std::optional<Algorithm> algorithmFromName(const QString &name)
{
    // In lower case and without what separates the parts: "SHA-256", "sha_1",
    // "SHA2-512" as openssl writes it, "sha256sum" as the tool is called and
    // "SHA256SUMS" as the list of a download is often named.
    QString key;
    for (const QChar c : name) {
        if (!c.isSpace() && c != QLatin1Char('-') && c != QLatin1Char('_')) {
            key += c.toLower();
        }
    }
    if (key.endsWith(QLatin1String("s"))) {
        key.chop(1);
    }
    if (key.endsWith(QLatin1String("sum"))) {
        key.chop(3);
    }
    if (key == QLatin1String("md5")) {
        return Algorithm::Md5;
    }
    if (key == QLatin1String("sha1")) {
        return Algorithm::Sha1;
    }
    if (key == QLatin1String("sha256") || key == QLatin1String("sha2256")) {
        return Algorithm::Sha256;
    }
    if (key == QLatin1String("sha512") || key == QLatin1String("sha2512")) {
        return Algorithm::Sha512;
    }
    if (key == QLatin1String("crc") || key == QLatin1String("crc32") || key == QLatin1String("sfv")) {
        return Algorithm::Crc32;
    }
    return std::nullopt;
}

int hashLength(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::Crc32:
        return 8;
    case Algorithm::Md5:
        return 32;
    case Algorithm::Sha1:
        return 40;
    case Algorithm::Sha256:
        return 64;
    case Algorithm::Sha512:
        return 128;
    }
    return 0;
}

std::optional<Algorithm> algorithmForLength(int length)
{
    for (Algorithm algorithm : {Algorithm::Crc32, Algorithm::Md5, Algorithm::Sha1, Algorithm::Sha256,
                                Algorithm::Sha512}) {
        if (hashLength(algorithm) == length) {
            return algorithm;
        }
    }
    return std::nullopt;
}

QString statusName(Status status)
{
    switch (status) {
    case Status::Ok:
        return tr("OK");
    case Status::Different:
        return tr("Different");
    case Status::Missing:
        return tr("Missing");
    case Status::Unreadable:
        return tr("Unreadable");
    case Status::NotChecked:
        return tr("Not checked");
    }
    return QString();
}

bool hashFile(const QString &path, Algorithm algorithm, QString *hash, QString *error, Progress *progress)
{
    hash->clear();
    error->clear();

    // Asked for before anything is opened: a folder or a device may open, but
    // doesn't read as a file (a pipe would wait for a writer for ever).
    const QFileInfo info(path);
    if (!info.exists()) {
        *error = tr("There is no file with this name.");
        return false;
    }
    if (!info.isFile()) {
        *error = info.isDir() ? tr("This is a folder, not a file.") : tr("This is not a file that can be read.");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    const qint64 total = file.size();
    if (progress && !progress->bytes(0, total)) {
        return false;
    }

    Hasher hasher(algorithm);
    QByteArray buffer(int(kChunkBytes), Qt::Uninitialized);
    qint64 done = 0;
    while (true) {
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0) {
            *error = file.errorString();
            return false;
        }
        if (read == 0) {
            break;
        }
        hasher.add(buffer.constData(), read);
        done += read;
        if (progress && !progress->bytes(done, total)) {
            return false;
        }
    }
    *hash = hasher.hex();
    return true;
}

ChecksumFile parse(const QString &text)
{
    ChecksumFile file;
    QString body = text;
    // As Windows editors write at the start of a text file.
    if (body.startsWith(QChar(0xFEFF))) {
        body.remove(0, 1);
    }

    for (const QString &raw : body.split('\n')) {
        // Trimming takes the "\r" of a Windows line ending too.
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        // "MD5 (name) = hash": what md5 and openssl print. The name says the
        // algorithm, so the hash has to be as long as that one's.
        const QRegularExpressionMatch tagged = kTaggedLine.match(line);
        if (tagged.hasMatch()) {
            const QString name = tagged.captured(2).trimmed();
            const QString hash = tagged.captured(3).toLower();
            const std::optional<Algorithm> byName = algorithmFromName(tagged.captured(1));
            if (!name.isEmpty() && byName && algorithmForLength(int(hash.size())) == byName) {
                file.entries.append(entry(name, hash, *byName));
                continue;
            }
        }

        const qsizetype firstSpace = line.indexOf(kSpace);
        const QString first = firstSpace < 0 ? line : line.left(firstSpace);
        const QString after = firstSpace < 0 ? QString() : line.mid(firstSpace).trimmed();

        // sha256sum and md5sum: the hash, two spaces, the file; one space and
        // a "*" for a file read in binary mode.
        std::optional<Algorithm> algorithm = algorithmForHex(first);
        QString name = after.startsWith(QLatin1Char('*')) ? after.mid(1) : after;
        if (algorithm && !name.isEmpty()) {
            file.entries.append(entry(name, first.toLower(), *algorithm));
            continue;
        }

        // An SFV file: the file, a space, the CRC. Some tools write MD5 files
        // this way round as well.
        const qsizetype lastSpace = line.lastIndexOf(kSpace);
        const QString last = lastSpace < 0 ? QString() : line.mid(lastSpace + 1);
        name = lastSpace < 0 ? QString() : line.left(lastSpace).trimmed();
        algorithm = algorithmForHex(last);
        if (algorithm && !name.isEmpty()) {
            file.entries.append(entry(name, last.toLower(), *algorithm));
            continue;
        }

        ++file.unread;
    }

    // One algorithm for the whole list, when they are all of the same kind:
    // what the header says, or what their lengths are alone.
    for (const Entry &entry : file.entries) {
        if (!file.algorithm) {
            file.algorithm = entry.algorithm;
        } else if (*file.algorithm != entry.algorithm) {
            file.algorithm.reset();
            break;
        }
    }
    return file;
}

bool read(const QString &path, ChecksumFile *file, QString *error)
{
    *file = ChecksumFile();
    error->clear();

    const QFileInfo info(path);
    if (info.isDir()) {
        *error = tr("This is a folder, not a file.");
        return false;
    }
    if (!info.exists()) {
        *error = tr("There is no file with this name.");
        return false;
    }
    if (info.size() > kMaxListBytes) {
        *error = tr("This file is too big to be a list of checksums.");
        return false;
    }
    QFile handle(path);
    if (!handle.open(QIODevice::ReadOnly)) {
        *error = handle.errorString();
        return false;
    }
    const QByteArray bytes = handle.readAll();
    if (handle.error() != QFileDevice::NoError) {
        *error = handle.errorString();
        return false;
    }

    *file = parse(decode(bytes));
    if (file->entries.isEmpty()) {
        *error = tr("This file lists no checksums.");
        return false;
    }
    file->path = info.absoluteFilePath();
    // The algorithm is not written in these files; the length of the hashes says which it is.
    const Algorithm first = file->entries.first().algorithm;
    for (const Entry &entry : file->entries) {
        if (entry.algorithm != first) {
            return true;
        }
    }
    file->algorithm = first;
    return true;
}

bool isChecksumFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        // There is no file, so there is no checksum file either.
        return false;
    }
    // The name says: ".md5", ".sha256", ".sfv", "SHA256SUMS".
    if (algorithmFromName(info.suffix())) {
        return true;
    }
    // A name that doesn't ("SHA256SUMS", "checksums.txt"): a small file that reads as a list.
    if (info.size() > kLookInsideBytes) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const ChecksumFile inside = parse(decode(file.read(kLookInsideBytes)));
    return !inside.entries.isEmpty() && inside.unread == 0;
}

QList<Item> itemsIn(const QString &folder, const QList<Entry> &entries)
{
    QList<Item> items;
    for (const Entry &entry : entries) {
        items.append(Item{entry.name, filePathIn(folder, entry.name), entry.algorithm, entry.hash});
    }
    return items;
}

bool check(const QList<Item> &items, QList<Check> *checks, Progress *progress)
{
    checks->clear();
    for (const Item &item : items) {
        checks->append(Check{item, Status::NotChecked, QString(), QString()});
    }

    // How much there is to read, for the progress.
    QList<QFileInfo> infos;
    qint64 total = 0;
    for (const Check &check : *checks) {
        infos.append(QFileInfo(check.item.path));
        if (infos.last().isFile()) {
            total += infos.last().size();
        }
    }

    qint64 done = 0;
    for (int i = 0; i < checks->size(); ++i) {
        Check &check = (*checks)[i];
        const QFileInfo &info = infos.at(i);
        if (!info.exists()) {
            check.status = Status::Missing;
        } else if (!info.isFile()) {
            check.status = Status::Unreadable;
            check.error = info.isDir() ? tr("This is a folder, not a file.")
                                       : tr("This is not a file that can be read.");
        } else {
            Counting counting(progress, done, total);
            const bool hashed = hashFile(check.item.path, check.item.algorithm, &check.actual, &check.error,
                                         progress ? &counting : nullptr);
            if (!hashed && check.error.isEmpty()) {
                // Stopped: this file and the rest are not looked at.
                return false;
            }
            if (hashed) {
                done += info.size();
                check.status = check.item.expected.isEmpty()
                                       || QString::compare(check.actual, check.item.expected, Qt::CaseInsensitive) == 0
                               ? Status::Ok
                               : Status::Different;
            } else {
                check.status = Status::Unreadable;
            }
        }
        if (progress && !progress->checked(check, i + 1, int(checks->size()))) {
            return false;
        }
    }
    return true;
}

QString hashIn(const QString &text)
{
    // Without the spaces, as a hash comes out of a PDF that broke it over
    // lines or set it in groups.
    QString digits;
    for (const QChar c : text) {
        if (!c.isSpace()) {
            digits += c;
        }
    }
    if (isHex(digits) && algorithmForLength(int(digits.size()))) {
        return digits.toLower();
    }

    // A line copied from a checksum file, or from what a tool printed. (Of
    // several lines, or a hash hidden in other text, the first one of a known
    // length is taken below; a CRC is too short to tell from anything else.)
    const ChecksumFile one = parse(text);
    if (one.entries.size() == 1 && one.unread == 0) {
        return one.entries.first().hash;
    }

    // Only digits, but of no length that is a hash: that is all there is.
    if (isHex(digits)) {
        return digits.toLower();
    }

    auto runs = kHexRun.globalMatch(text);
    while (runs.hasNext()) {
        const QRegularExpressionMatch run = runs.next();
        const std::optional<Algorithm> algorithm = algorithmForLength(int(run.capturedLength()));
        if (algorithm && *algorithm != Algorithm::Crc32) {
            return run.captured().toLower();
        }
    }
    return QString();
}

Comparison compare(const QString &computed, const QString &expected)
{
    const QString wanted = hashIn(expected);
    // A hash of another kind is no mismatch: nothing was computed to compare it with.
    if (wanted.isEmpty() || wanted.size() != computed.size()) {
        return Comparison::NotHash;
    }
    return QString::compare(wanted, computed, Qt::CaseInsensitive) == 0 ? Comparison::Match
                                                                        : Comparison::Different;
}

QString summary(const QList<Check> &checks)
{
    int ok = 0;
    int different = 0;
    int missing = 0;
    int unreadable = 0;
    int notChecked = 0;
    for (const Check &check : checks) {
        switch (check.status) {
        case Status::Ok:
            ++ok;
            break;
        case Status::Different:
            ++different;
            break;
        case Status::Missing:
            ++missing;
            break;
        case Status::Unreadable:
            ++unreadable;
            break;
        case Status::NotChecked:
            ++notChecked;
            break;
        }
    }
    QStringList parts;
    const auto add = [&parts](int count, const QString &what) {
        if (count > 0) {
            parts << QStringLiteral("%1 %2").arg(count).arg(what);
        }
    };
    add(ok, tr("OK"));
    add(different, tr("different"));
    add(missing, tr("missing"));
    add(unreadable, tr("unreadable"));
    add(notChecked, tr("not checked"));
    return parts.join(QStringLiteral(", "));
}

QString toText(const QList<Entry> &entries, Format format)
{
    QStringList lines;
    for (const Entry &entry : entries) {
        lines << (format == Format::Sfv ? entry.name + QLatin1Char(' ') + entry.hash.toUpper()
                                        : entry.hash + QStringLiteral("  ") + entry.name);
    }
    QString text = lines.join(QLatin1Char('\n'));
    if (!text.isEmpty()) {
        text += QLatin1Char('\n');
    }
    return text;
}

bool write(const QString &path, const QList<Entry> &entries, QString *error)
{
    error->clear();
    for (const Entry &entry : entries) {
        if (entry.name.contains(QLatin1Char('\n')) || entry.name.contains(QLatin1Char('\r'))) {
            *error = tr("A name with a line break in it cannot be in a checksum file.");
            return false;
        }
    }

    bool allCrc = !entries.isEmpty();
    for (const Entry &entry : entries) {
        allCrc = allCrc && entry.algorithm == Algorithm::Crc32;
    }
    const QByteArray bytes = toText(entries, allCrc ? Format::Sfv : Format::Sum).toUtf8();

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

}  // namespace Checksums
