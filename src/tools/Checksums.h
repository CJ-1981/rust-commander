#pragma once

#include <QList>
#include <QString>

#include <optional>

// Hashes of files, and the files that list such hashes, without any user
// interface. Everything here blocks while it goes; what can take long reports
// through `Progress` and stops when that says so, so that it can run on a
// worker thread (see ChecksumWindow).
namespace Checksums {

enum class Algorithm {
    Crc32,
    Md5,
    Sha1,
    Sha256,
    Sha512,
};

// What checking a file against a hash found.
enum class Status {
    Ok,          // the file has that hash
    Different,   // the file is there, but has another hash
    Missing,     // there is no file with that name
    Unreadable,  // something is there, but it is not a file that can be read
    NotChecked,  // not looked at, because the check was stopped before it
};

// How a checksum file is written.
enum class Format {
    Sum,  // "<hash>  <name>", as sha256sum and md5sum write
    Sfv,  // "<name> <CRC>", in capitals
};

enum class Comparison {
    Match,      // the same hash
    Different,  // a hash of the same kind, but not this one
    NotHash,    // no hash of that kind in the text
};

// One line of a checksum file.
struct Entry {
    QString name;      // the file, relative to the folder the checksum file is in
    QString hash;      // in lower case
    Algorithm algorithm = Algorithm::Sha256;
};

// A file to hash, and the hash a checksum file says it has, if there is one.
struct Item {
    QString name;      // what the file is called in the result
    QString path;      // the file itself
    Algorithm algorithm = Algorithm::Sha256;
    QString expected;  // in lower case; empty to only find the hash
};

// What checking one file found.
struct Check {
    Item item;
    Status status = Status::NotChecked;
    QString actual;    // the hash the file has; empty unless it was read
    QString error;     // why a file that is there could not be read
};

// Reports as the work goes. Returning false stops it.
class Progress {
public:
    virtual ~Progress() = default;

    // Bytes of the file being read: so many out of its size. Called before the
    // first one is read and after every chunk.
    virtual bool bytes(qint64 done, qint64 total) = 0;
    // The `done`th of `count` files was looked at, whatever was found.
    virtual bool checked(const Check &check, int done, int count) { return true; }
};

// What a checksum file lists.
struct ChecksumFile {
    QString path;
    QList<Entry> entries;  // in the order of the file
    int unread = 0;        // lines that are neither a comment nor a checksum
    // The one algorithm all its hashes are in, told by their length and by the
    // name a line may start with; nothing when they are mixed.
    std::optional<Algorithm> algorithm;
};

// "MD5", "SHA-256", ...
QString algorithmName(Algorithm algorithm);
// What a checksum file of this algorithm ends in: "md5", "sha256", "sfv".
QString fileSuffix(Algorithm algorithm);
// The algorithm a name stands for: "md5", "SHA-256", "SHA2-512", "sha256sum",
// or "sfv" for the files that list CRC32.
std::optional<Algorithm> algorithmFromName(const QString &name);
// Number of hex digits in a hash of this algorithm.
int hashLength(Algorithm algorithm);
// The algorithm whose hashes have `length` hex digits; nothing for any other length.
std::optional<Algorithm> algorithmForLength(int length);
QString statusName(Status status);

// The hash of the file at `path`, in lower case. The file is read in chunks,
// so a file may be bigger than the memory. False when `error` says why it
// could not be read, and when `progress` stopped it (then `error` stays empty).
bool hashFile(const QString &path, Algorithm algorithm, QString *hash, QString *error,
              Progress *progress = nullptr);

// A hash as typed or pasted: in lower case, without the spaces in and around
// it. A line copied from a checksum file, or from what a tool printed, gives
// the hash in it. Empty when there is none.
QString hashIn(const QString &text);
// Whether `expected`, as typed or pasted, is the hash `computed` (in lower case).
Comparison compare(const QString &computed, const QString &expected);

// Reads the text of a checksum file: "<hash>  <name>" and "<hash> *<name>" as
// sha256sum and md5sum write, "MD5 (<name>) = <hash>" as the tools on macOS
// print, and "<name> <CRC>" in an SFV file.
ChecksumFile parse(const QString &text);
// Reads the checksum file at `path`. False when it could not be read or lists
// nothing; `error` says which.
bool read(const QString &path, ChecksumFile *file, QString *error);
// Whether this is a checksum file: its name says so (".md5", ".sha256",
// ".sfv"), or else a small file reads as one.
bool isChecksumFile(const QString &path);

// The files of a checksum file's entries, as they are in `folder`. The names
// are taken as they are, with a leading "./" left out; one written on Windows,
// with backslashes, is looked for with slashes too.
QList<Item> itemsIn(const QString &folder, const QList<Entry> &entries);

// Hashes the files one after the other and compares each with the hash that is
// expected of it. `checks` holds one for every file from the start, so a check
// that was stopped leaves the rest NotChecked. False when `progress` stopped it.
bool check(const QList<Item> &items, QList<Check> *checks, Progress *progress = nullptr);

// "18 OK, 1 different, 2 missing": only what there is.
QString summary(const QList<Check> &checks);

// `entries` as the text of a checksum file, every line ended with a line feed.
QString toText(const QList<Entry> &entries, Format format);
// Writes the entries to `path`, as an SFV file when they are all CRC32 and else
// as sha256sum writes. False when a name has a line break in it, or the file
// could not be written; `error` says which.
bool write(const QString &path, const QList<Entry> &entries, QString *error);

}  // namespace Checksums
