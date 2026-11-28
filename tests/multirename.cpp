#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <memory>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "app/Theme.h"
#include "helpers.h"
#include "ops/FileOps.h"
#include "tools/MultiRenameWindow.h"
#include "tools/RenameRules.h"

namespace {

using RenameRules::Case;
using RenameRules::Result;
using RenameRules::Settings;
using RenameRules::Source;

// 7 March 2024, five past nine in the morning.
const QDateTime kDate(QDate(2024, 3, 7), QTime(9, 5, 31));

// The settings with a mask, a find-and-replace and so on, as a test gives them.
struct Rules {
    QString mask = "[N].[E]";
    QString find;
    QString replace;
    bool regularExpression = false;
    Case caseMode = Case::Unchanged;
    int counterStart = 1;
    int counterStep = 1;
    int counterWidth = 1;
    bool underscoresToSpaces = false;

    operator Settings() const
    {
        Settings settings;
        settings.mask = mask;
        settings.find = find;
        settings.replace = replace;
        settings.regularExpression = regularExpression;
        settings.caseMode = caseMode;
        settings.counterStart = counterStart;
        settings.counterStep = counterStep;
        settings.counterWidth = counterWidth;
        settings.underscoresToSpaces = underscoresToSpaces;
        return settings;
    }
};

// Why FileOps::isAllowedName refuses `name`; the test fails when it doesn't.
QString notAllowed(const QString &name)
{
    QString reason;
    if (FileOps::isAllowedName(name, &reason)) {
        reason = QStringLiteral("%1 is an allowed name after all").arg(name);
    }
    return reason;
}

// What the rules make of `name`: the new name, or "<why there is none>".
QString newName(const QString &name, const Rules &rules, int index = 0, const QDateTime &modified = {})
{
    const Result result = RenameRules::newName(Source{name, modified, index}, rules);
    return result.ok() ? result.name : QStringLiteral("<%1>").arg(result.error);
}

// Files with these names in `folder`, empty; their paths, in the same order.
QStringList makeFiles(const QString &folder, const QStringList &names)
{
    QStringList paths;
    for (const QString &name : names) {
        const QString path = folder + "/" + name;
        writeFile(path, name.toUtf8());
        paths.append(path);
    }
    return paths;
}

// What `folder` holds, sorted by QString's order, in the form it is on disk.
QStringList folderHolds(const QString &folder)
{
    QStringList names = QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                               QDir::NoSort);
    std::sort(names.begin(), names.end());
    return names;
}

// The new names of the rows, top to bottom.
QStringList newNames(const QList<MultiRenameWindow::Row> &rows)
{
    QStringList names;
    for (const MultiRenameWindow::Row &row : rows) {
        names.append(row.newName);
    }
    return names;
}

// The row of the file that is called `newName` after the rules, or after a rename.
MultiRenameWindow::Row rowFor(const QList<MultiRenameWindow::Row> &rows, const QString &newName)
{
    for (const MultiRenameWindow::Row &row : rows) {
        if (row.newName == newName) {
            return row;
        }
    }
    return MultiRenameWindow::Row();
}

using State = MultiRenameWindow::State;

// A folder with files in it, and a window over them.
struct Fixture {
    QTemporaryDir root;
    QString folder;
    QString other;  // a second folder, empty
    ScriptedDialogs dialogs;

    Fixture()
        : folder(root.path() + "/one")
        , other(root.path() + "/two")
    {
        makeFolder(folder);
        makeFolder(other);
    }

    // Files in the first folder, and a window over them.
    std::unique_ptr<MultiRenameWindow> window(const QStringList &names)
    {
        return open(makeFiles(folder, names));
    }

    // A window over paths that are already there.
    std::unique_ptr<MultiRenameWindow> open(const QStringList &paths)
    {
        // The window deletes itself when it is closed, which the unique_ptr would then delete again.
        auto *window = new MultiRenameWindow(dialogs);
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        window->setFiles(paths);
        return std::unique_ptr<MultiRenameWindow>(window);
    }
};

}  // namespace

class TestMultiRename : public QObject {
    Q_OBJECT

private slots:
    void init();
    void nameAndExtension();
    void partsOfTheName();
    void partsOfTheExtension();
    void counter();
    void counterWithItsOwnNumbers();
    void dateAndTime();
    void caseInTheMask();
    void bracketsAndWhatIsNotAPlaceholder();

    void findAndReplaceLiterally();
    void findAndReplaceWithARegularExpression();
    void whatIsWrongWithTheSettings();

    void caseRules();
    void underscores();
    void namesThatCannotBeUsed();
    void theRulesInOrder();

    void showsWhatWouldHappen();
    void theDateOfTheFile();
    void thePreviewFollowsTheTyping();
    void theSameNewNameForTwoFiles();
    void aNameThatIsTaken();
    void whatOnlySomeFilesCanUse();
    void renamingAndUndo();
    void undoStepByStep();
    void aFileThatFailsDoesNotStopTheOthers();
    void anUndoThatFailsCanBeTriedAgain();
    void filesThatAreGoneAndFilesSetAgain();
    void openedForASelection();
};

// The window is shown with the app's light look, as a user with that setting sees it.
void TestMultiRename::init()
{
    Theme::apply(Theme::Mode::Light);
}

void TestMultiRename::nameAndExtension()
{
    // The default mask leaves every name as it is, with or without an extension.
    for (const QString &name : {QString("photo.jpg"), QString("README"), QString(".hidden"),
                                QString("archive.tar.gz"), QString("name."), QString(".a")}) {
        QCOMPARE(newName(name, Rules()), name);
    }

    QCOMPARE(newName("photo.jpg", Rules{.mask = "[N]_copy.[E]"}), QStringLiteral("photo_copy.jpg"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[N]"}), QStringLiteral("photo"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[E]"}), QStringLiteral("jpg"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[E].[N]"}), QStringLiteral("jpg.photo"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "holiday [N]"}), QStringLiteral("holiday photo"));
    // No extension: nothing is put after the name, and no dot either.
    QCOMPARE(newName("README", Rules{.mask = "[N]_copy.[E]"}), QStringLiteral("README_copy"));
    QCOMPARE(newName("README", Rules{.mask = "[N]x.[E]"}), QStringLiteral("READMEx"));
    QCOMPARE(newName(".hidden", Rules{.mask = "[N]_[C]"}), QStringLiteral(".hidden_1"));
    // Only the last part is the extension.
    QCOMPARE(newName("archive.tar.gz", Rules{.mask = "[N] [E]"}), QStringLiteral("archive.tar gz"));
    QCOMPARE(newName("archive.tar.gz", Rules{.mask = "[N]-[E]"}), QStringLiteral("archive.tar-gz"));
}

void TestMultiRename::partsOfTheName()
{
    const QString name = "abcde.txt";
    QCOMPARE(newName(name, Rules{.mask = "[N1]"}), QStringLiteral("a"));
    QCOMPARE(newName(name, Rules{.mask = "[N2]"}), QStringLiteral("b"));
    QCOMPARE(newName(name, Rules{.mask = "[N5]"}), QStringLiteral("e"));
    QCOMPARE(newName(name, Rules{.mask = "[N-1]"}), QStringLiteral("e"));
    QCOMPARE(newName(name, Rules{.mask = "[N-2]"}), QStringLiteral("d"));
    QCOMPARE(newName(name, Rules{.mask = "[N1-3]"}), QStringLiteral("abc"));
    QCOMPARE(newName(name, Rules{.mask = "[N2-4]"}), QStringLiteral("bcd"));
    QCOMPARE(newName(name, Rules{.mask = "[N1-5]"}), QStringLiteral("abcde"));
    QCOMPARE(newName(name, Rules{.mask = "[N2-]"}), QStringLiteral("bcde"));
    QCOMPARE(newName(name, Rules{.mask = "[N3-]"}), QStringLiteral("cde"));
    QCOMPARE(newName(name, Rules{.mask = "[N-3-]"}), QStringLiteral("cde"));
    QCOMPARE(newName(name, Rules{.mask = "[N2--2]"}), QStringLiteral("bcd"));
    QCOMPARE(newName(name, Rules{.mask = "[N-4--2]"}), QStringLiteral("bcd"));
    QCOMPARE(newName(name, Rules{.mask = "[N2,3]"}), QStringLiteral("bcd"));
    QCOMPARE(newName(name, Rules{.mask = "[N-4,3]"}), QStringLiteral("bcd"));
    // In a mask with more around it.
    QCOMPARE(newName(name, Rules{.mask = "[N1-2]-[N-1].[E]"}), QStringLiteral("ab-e.txt"));

    // A name shorter than the range: what there is.
    const QString shortName = "ab.txt";
    QCOMPARE(newName(shortName, Rules{.mask = "[N1-3]"}), QStringLiteral("ab"));
    QCOMPARE(newName(shortName, Rules{.mask = "[N2-5]"}), QStringLiteral("b"));
    QCOMPARE(newName(shortName, Rules{.mask = "[N-5-]"}), QStringLiteral("ab"));
    QCOMPARE(newName(shortName, Rules{.mask = "[N2,5]"}), QStringLiteral("b"));
    QCOMPARE(newName(shortName, Rules{.mask = "[N4-]x"}), QStringLiteral("x"));
    QCOMPARE(newName(shortName, Rules{.mask = "[N4]"}), QStringLiteral("<The new name would be empty.>"));
    QCOMPARE(newName(shortName, Rules{.mask = "[N-5]"}), QStringLiteral("<The new name would be empty.>"));

    // Korean: a syllable is a character, as a reader counts them.
    const QString korean = QString::fromUtf8("한국어 파일.txt");
    QCOMPARE(newName(korean, Rules{.mask = "[N1-2]"}), QString::fromUtf8("한국"));
    QCOMPARE(newName(korean, Rules{.mask = "[N2,2]"}), QString::fromUtf8("국어"));
    QCOMPARE(newName(korean, Rules{.mask = "[N5-]"}), QString::fromUtf8("파일"));
    QCOMPARE(newName(korean, Rules{.mask = "[N-2]"}), QString::fromUtf8("파"));
    QCOMPARE(newName(korean, Rules{.mask = "[N-1]"}), QString::fromUtf8("일"));
    // The same syllables, written as jamo: still two characters.
    const QString jamo = QString::fromUtf8("한국어 파일.txt").normalized(QString::NormalizationForm_D);
    QVERIFY(jamo != korean);
    QCOMPARE(newName(jamo, Rules{.mask = "[N1-2]"}).normalized(QString::NormalizationForm_C),
             QString::fromUtf8("한국"));
    // A letter with its accent in a separate character is one.
    const QString accent = QString::fromUtf8("cafe\u0301.txt");
    QCOMPARE(accent.size(), 9);
    QCOMPARE(newName(accent, Rules{.mask = "[N3]"}), QStringLiteral("f"));
    QCOMPARE(newName(accent, Rules{.mask = "[N4]"}), QString::fromUtf8("e\u0301"));
    // Counted as two, the accent alone would be the fifth character.
    QCOMPARE(newName(accent, Rules{.mask = "[N5]"}), QStringLiteral("<The new name would be empty.>"));
    QCOMPARE(newName(accent, Rules{.mask = "[N-5-]"}), QString::fromUtf8("cafe\u0301"));
}

void TestMultiRename::partsOfTheExtension()
{
    const QString name = "photo.jpeg";
    QCOMPARE(newName(name, Rules{.mask = "[N].[E1]"}), QStringLiteral("photo.j"));
    QCOMPARE(newName(name, Rules{.mask = "[N].[E1-3]"}), QStringLiteral("photo.jpe"));
    QCOMPARE(newName(name, Rules{.mask = "[N].[E2-3]"}), QStringLiteral("photo.pe"));
    QCOMPARE(newName(name, Rules{.mask = "[N].[E-1]"}), QStringLiteral("photo.g"));
    QCOMPARE(newName(name, Rules{.mask = "[N].[E2-]"}), QStringLiteral("photo.peg"));
    // The extension as it is written, and none where there is none.
    QCOMPARE(newName("photo.JPEG", Rules{.mask = "[L][N].[n][E]"}), QStringLiteral("photo.JPEG"));
    QCOMPARE(newName("photo.JPEG", Rules{.mask = "[L][N].[E]"}), QStringLiteral("photo.jpeg"));
    QCOMPARE(newName("README", Rules{.mask = "[N].[E2-]"}), QStringLiteral("README"));
    QCOMPARE(newName("README", Rules{.mask = "[E2-]x"}), QStringLiteral("x"));
}

void TestMultiRename::counter()
{
    const Rules rules{.mask = "[C].[E]"};
    QCOMPARE(newName("a.txt", rules, 0), QStringLiteral("1.txt"));
    QCOMPARE(newName("b.txt", rules, 1), QStringLiteral("2.txt"));
    QCOMPARE(newName("c.txt", rules, 2), QStringLiteral("3.txt"));

    QCOMPARE(newName("a.txt", Rules{.mask = "[C]", .counterStart = 10}, 0), QStringLiteral("10"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C]", .counterStart = 10}, 1), QStringLiteral("11"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C]", .counterStart = 0}, 2), QStringLiteral("2"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C]", .counterStart = 5, .counterStep = 2}, 3),
             QStringLiteral("11"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C]", .counterStart = 10, .counterStep = -1}, 3),
             QStringLiteral("7"));

    // Padded with zeros to the width; wider numbers are not cut.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C]", .counterWidth = 3}, 0), QStringLiteral("001"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C]", .counterWidth = 3}, 11), QStringLiteral("012"));
    QCOMPARE(newName("c.txt", Rules{.mask = "[C]", .counterStart = 99, .counterWidth = 2}, 1),
             QStringLiteral("100"));
    QCOMPARE(newName("d.txt", Rules{.mask = "[C]", .counterStart = 0, .counterStep = -1, .counterWidth = 3}, 1),
             QStringLiteral("-001"));

    // Where it is asked for.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C]_[N]"}), QStringLiteral("1_a"));
    QCOMPARE(newName("a.txt", Rules{.mask = "photo_[C:2].[E]"}), QStringLiteral("photo_01.txt"));
    QCOMPARE(newName("a", Rules{.mask = "[N] ([C])"}), QStringLiteral("a (1)"));
    // Every [C] of a name has the number of that file.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C]-[C]"}, 4), QStringLiteral("5-5"));
}

void TestMultiRename::counterWithItsOwnNumbers()
{
    // [C10] starts at 10.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C10].[E]"}, 0), QStringLiteral("10.txt"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C10].[E]"}, 2), QStringLiteral("12.txt"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C10].[E]", .counterStart = 7}, 2), QStringLiteral("12.txt"));

    // [C+10] starts ten above where the settings start, at 11 by default.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C+10]"}, 0), QStringLiteral("11"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C+10]"}, 1), QStringLiteral("12"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C+10]", .counterStart = 5}, 0), QStringLiteral("15"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C-2]"}, 0), QStringLiteral("-1"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C-2]"}, 1), QStringLiteral("0"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C-2]"}, 2), QStringLiteral("1"));

    // [C10:3] starts at 10 and is three digits wide.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C10:3]"}, 0), QStringLiteral("010"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C10:3]"}, 1), QStringLiteral("011"));
    QCOMPARE(newName("c.txt", Rules{.mask = "[C10:3]"}, 123), QStringLiteral("133"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C:3]"}, 0), QStringLiteral("001"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C]", .counterWidth = 4}, 0), QStringLiteral("0001"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C:2]", .counterWidth = 4}, 12), QStringLiteral("13"));

    // Start, step and width at once: 10, 15, 20.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C10+5:3]"}, 0), QStringLiteral("010"));
    QCOMPARE(newName("b.txt", Rules{.mask = "[C10+5:3]"}, 1), QStringLiteral("015"));
    QCOMPARE(newName("c.txt", Rules{.mask = "[C10+5:3]"}, 2), QStringLiteral("020"));
    QCOMPARE(newName("c.txt", Rules{.mask = "[C+2-3:2]"}, 1), QStringLiteral("00"));
    QCOMPARE(newName("d.txt", Rules{.mask = "[C+2-3:2]"}, 2), QStringLiteral("-03"));

    // Counters side by side.
    QCOMPARE(newName("a.txt", Rules{.mask = "[C]-[C+1]"}, 0), QStringLiteral("1-2"));
    QCOMPARE(newName("a.txt", Rules{.mask = "[C:2]-[C100:3]"}, 4), QStringLiteral("05-104"));
}

void TestMultiRename::dateAndTime()
{
    const QString name = "photo.jpg";
    QCOMPARE(newName(name, Rules{.mask = "[Y]-[M]-[D] [h].[m].[s].[E]"}, 0, kDate),
             QStringLiteral("2024-03-07 09.05.31.jpg"));
    QCOMPARE(newName(name, Rules{.mask = "[Y]"}, 0, kDate), QStringLiteral("2024"));
    QCOMPARE(newName(name, Rules{.mask = "[M]"}, 0, kDate), QStringLiteral("03"));
    QCOMPARE(newName(name, Rules{.mask = "[D]"}, 0, kDate), QStringLiteral("07"));
    QCOMPARE(newName(name, Rules{.mask = "[h]"}, 0, kDate), QStringLiteral("09"));
    QCOMPARE(newName(name, Rules{.mask = "[m]"}, 0, kDate), QStringLiteral("05"));
    QCOMPARE(newName(name, Rules{.mask = "[s]"}, 0, kDate), QStringLiteral("31"));
    // The 24-hour clock.
    const QDateTime evening(QDate(2024, 12, 31), QTime(18, 0, 7));
    QCOMPARE(newName(name, Rules{.mask = "[D][M][Y]_[h][m][s]"}, 0, evening),
             QStringLiteral("31122024_180007"));
    // A file's date is what it is, wherever the rules look at it from.
    QCOMPARE(newName(name, Rules{.mask = "[Y] [N]"}, 0, kDate), QStringLiteral("2024 photo"));
}

void TestMultiRename::caseInTheMask()
{
    // [U], [L] and [F] go for what follows them; [n] goes back to as it was.
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[U][N] [n][E]"}), QStringLiteral("PHOTO jpg"));
    QCOMPARE(newName("PHOTO.JPG", Rules{.mask = "[L][N].[n][E]"}), QStringLiteral("photo.JPG"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[U]ab [n]Cd"}), QStringLiteral("AB Cd"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[N]x[L][N]"}), QStringLiteral("photoxphoto"));
    QCOMPARE(newName("My Holiday.jpg", Rules{.mask = "[F][N] [n][E]"}), QStringLiteral("My Holiday jpg"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[F]my holiday [n]PHOTOS"}), QStringLiteral("My Holiday PHOTOS"));
    // A word that goes from one placeholder into the next.
    QCOMPARE(newName("holiday.jpg", Rules{.mask = "[F]my [N]"}), QStringLiteral("My Holiday"));
    QCOMPARE(newName("holiday.jpg", Rules{.mask = "[F]my[N]"}), QStringLiteral("Myholiday"));
    // What comes after the dot of the extension is a new word only when a space says so.
    QCOMPARE(newName("holiday.jpg", Rules{.mask = "[F]my [N].[E]"}), QStringLiteral("My Holiday.jpg"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[U][N].[L][E]"}), QStringLiteral("PHOTO.jpg"));
    // With a counter, and in upper case, both.
    QCOMPARE(newName("a.jpg", Rules{.mask = "[U][N]_[C]"}, 3), QStringLiteral("A_4"));
}

void TestMultiRename::bracketsAndWhatIsNotAPlaceholder()
{
    // "[[]" is [ and "[]]" is ].
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[[]N]"}), QStringLiteral("[N]"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[[]N]"}), QStringLiteral("[N]"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[[]N]]"}), QStringLiteral("[N]]"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "a[]]b"}), QStringLiteral("a]b"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[[]"}), QStringLiteral("["));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[N]]"}), QStringLiteral("photo]"));
    // "[]" on its own is nothing.
    QCOMPARE(newName("photo.jpg", Rules{.mask = "a[]b"}), QStringLiteral("ab"));
    // Text without brackets is a name as it stands.
    QCOMPARE(newName("photo.jpg", Rules{.mask = "report 2024"}), QStringLiteral("report 2024"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "한국어"}), QString::fromUtf8("한국어"));

    // What isn't a placeholder is an error, and the same one for every file.
    for (const QString &mask : {QString("[X]"), QString("[x]"), QString("[n2]"), QString("[Y2]"), QString("[U2]"),
                                QString("[P]"), QString("[d]"), QString("[ N]"), QString("[N1-x]"), QString("[N0]"),
                                QString("[N-]"), QString("[N2,]"), QString("[N2,0]"), QString("[N2-5-7]"),
                                QString("[E1,]"), QString("[C:0]"), QString("[C5-]"), QString("[C+]"), QString("[C1:2:3]"),
                                QString("[C99999999999]"), QString("[N99999999999]"), QString("x[N"),
                                QString("[N][")}) {
        const Rules rules{.mask = mask};
        const QString problem = RenameRules::problem(rules);
        QVERIFY2(!problem.isEmpty(), qPrintable(mask));
        // Every file fails with it, before anything else about the file counts.
        const Result result = RenameRules::newName(Source{"a.txt", {}, 0}, rules);
        QVERIFY(!result.ok());
        QCOMPARE(result.error, problem);
        QVERIFY(result.name.isEmpty());
    }
    // An empty mask, or one that gives an empty name, is a problem of the file only.
    QVERIFY(RenameRules::problem(Rules{.mask = ""}).isEmpty());
    QCOMPARE(newName("a.txt", Rules{.mask = ""}), QStringLiteral("<The new name would be empty.>"));
}

void TestMultiRename::findAndReplaceLiterally()
{
    // Every time it is there, and in any case.
    QCOMPARE(newName("copy of a.txt", Rules{.find = "copy", .replace = "original"}),
             QStringLiteral("original of a.txt"));
    QCOMPARE(newName("IMG_01.jpg", Rules{.find = "img", .replace = "photo"}), QStringLiteral("photo_01.jpg"));
    QCOMPARE(newName("img_01.jpg", Rules{.find = "IMG", .replace = "photo"}), QStringLiteral("photo_01.jpg"));
    QCOMPARE(newName("a-b-a.txt", Rules{.find = "a", .replace = "x"}), QStringLiteral("x-b-x.txt"));
    // The replacement isn't searched again.
    QCOMPARE(newName("aa.txt", Rules{.find = "a", .replace = "aa"}), QStringLiteral("aaaa.txt"));
    QCOMPARE(newName("abab.txt", Rules{.find = "ab", .replace = "abab"}), QStringLiteral("abababab.txt"));
    // The extension too, and to nothing.
    QCOMPARE(newName("a.jpg", Rules{.find = "jpg", .replace = "png"}), QStringLiteral("a.png"));
    QCOMPARE(newName("photo.jpg", Rules{.find = "photo."}), QStringLiteral("jpg"));
    // Nothing found, or nothing to find: the name stays.
    QCOMPARE(newName("a.txt", Rules{.find = "zzz", .replace = "y"}), QStringLiteral("a.txt"));
    QCOMPARE(newName("a.txt", Rules{.replace = "y"}), QStringLiteral("a.txt"));
    // Korean.
    QCOMPARE(newName(QString::fromUtf8("복사 한.jpg"), Rules{.find = QString::fromUtf8("복사"),
                                                            .replace = QString::fromUtf8("사진")}),
             QString::fromUtf8("사진 한.jpg"));
    // After the mask.
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[N]-new.[E]", .find = "new", .replace = "old"}),
             QStringLiteral("photo-old.jpg"));
    // $ and other special characters mean nothing here.
    QCOMPARE(newName("a.txt", Rules{.find = "a", .replace = "$1 (x)"}), QStringLiteral("$1 (x).txt"));
    QCOMPARE(newName("a", Rules{.find = "a", .replace = ""}), QStringLiteral("<The new name would be empty.>"));
}

void TestMultiRename::findAndReplaceWithARegularExpression()
{
    // Groups in the replacement, as $1 and as ${name}.
    QCOMPARE(newName("2024-03-07 trip.jpg", Rules{.find = "^(\\d{4})-(\\d{2})-(\\d{2})", .replace = "$3.$2.$1",
                                              .regularExpression = true}),
             QStringLiteral("07.03.2024 trip.jpg"));
    QCOMPARE(newName("2024-03 trip.jpg", Rules{.find = "(?<y>\\d{4})-(?<m>\\d{2})", .replace = "${m}_${y}",
                                              .regularExpression = true}),
             QStringLiteral("03_2024 trip.jpg"));
    // A group that didn't match is empty, and $0 is the whole match.
    QCOMPARE(newName("a.txt", Rules{.find = "(x)?a", .replace = "$1b", .regularExpression = true}),
             QStringLiteral("b.txt"));
    QCOMPARE(newName("IMG_0001.jpg", Rules{.find = "\\d+", .replace = "[$0]", .regularExpression = true}),
             QStringLiteral("IMG_[0001].jpg"));
    // Every match, also one that is empty.
    QCOMPARE(newName("IMG_0001.jpg", Rules{.find = "\\d", .replace = "x", .regularExpression = true}),
             QStringLiteral("IMG_xxxx.jpg"));
    QCOMPARE(newName("a.txt", Rules{.find = "^", .replace = "x", .regularExpression = true}),
             QStringLiteral("xa.txt"));
    QCOMPARE(newName("a.txt", Rules{.find = "t(?=xt)", .replace = "T", .regularExpression = true}),
             QStringLiteral("a.Txt"));
    // The pattern is case sensitive until it says otherwise.
    QCOMPARE(newName("IMG_0001.jpg", Rules{.find = "img_(\\d+)", .replace = "photo_$1", .regularExpression = true}),
             QStringLiteral("IMG_0001.jpg"));
    QCOMPARE(newName("IMG_0001.jpg", Rules{.find = "(?i)img_(\\d+)", .replace = "photo_$1",
                                          .regularExpression = true}),
             QStringLiteral("photo_0001.jpg"));
    // Unicode: letters are letters, in Korean too.
    QCOMPARE(newName(QString::fromUtf8("한국어 파일.txt"), Rules{.find = "\\w+", .replace = "[$0]",
                                                              .regularExpression = true}),
             QString::fromUtf8("[한국어] [파일].[txt]"));
    // The whole name, and the rules around it.
    QCOMPARE(newName("DSC_1234 (2).jpg",
                     Rules{.mask = "[N]x.[E]", .find = "^(\\w+)_(\\d+).*", .replace = "$2-$1",
                           .regularExpression = true, .caseMode = Case::Upper}),
             QStringLiteral("1234-DSC"));
    QCOMPARE(newName("a_b.txt", Rules{.find = "^(.*)$", .replace = "$1 $1", .regularExpression = true,
                                      .underscoresToSpaces = true}),
             QStringLiteral("a b.txt a b.txt"));
    // A $ that is one: $$, or a $ with nothing that could be a group behind it.
    QCOMPARE(newName("a.txt", Rules{.find = "a", .replace = "$$1", .regularExpression = true}),
             QStringLiteral("$1.txt"));
    QCOMPARE(newName("a.txt", Rules{.find = "a", .replace = "x$", .regularExpression = true}),
             QStringLiteral("x$.txt"));
    QCOMPARE(newName("a.txt", Rules{.find = "a", .replace = "5 $ {x}", .regularExpression = true}),
             QStringLiteral("5 $ {x}.txt"));
    // A group followed by a digit, and the other way round.
    QCOMPARE(newName("a.txt", Rules{.find = "(a)", .replace = "${1}1", .regularExpression = true}),
             QStringLiteral("a1.txt"));
    QCOMPARE(newName("a.txt", Rules{.find = "(a)", .replace = "[$01]", .regularExpression = true}),
             QStringLiteral("[a].txt"));
    // Nothing found: the name stays.
    QCOMPARE(newName("a.txt", Rules{.find = "zz", .replace = "$0!", .regularExpression = true}),
             QStringLiteral("a.txt"));
}

void TestMultiRename::whatIsWrongWithTheSettings()
{
    QVERIFY(RenameRules::problem(Rules()).isEmpty());
    QVERIFY(RenameRules::problem(Rules{.find = "(a)", .replace = "$1", .regularExpression = true}).isEmpty());

    // A regular expression that isn't one.
    for (const QString &find : {QString("("), QString("[a-"), QString("a**"), QString("\\k<x>"), QString("(?<name>a)(?<name>b)")}) {
        const Rules rules{.find = find, .replace = "x", .regularExpression = true};
        const QString problem = RenameRules::problem(rules);
        QVERIFY2(!problem.isEmpty(), qPrintable(find));
        QCOMPARE(newName("a.txt", rules), QStringLiteral("<%1>").arg(problem));
        // Literal, the same text is just text.
        QVERIFY(RenameRules::problem(Rules{.find = find, .replace = "x"}).isEmpty());
    }

    // A group the expression doesn't have, in either way of writing it.
    for (const QString &replace : {QString("$3"), QString("${3}"), QString("$11"), QString("${name}"),
                                   QString("${2b}"), QString("${}"), QString("${1x}"), QString("$0 $9"),
                                   QString("a ${"), QString("$99999999999")}) {
        const Rules rules{.find = "(a)(?<b>c)?", .replace = replace, .regularExpression = true};
        QVERIFY2(!RenameRules::problem(rules).isEmpty(), qPrintable(replace));
        QCOMPARE(newName("abc.txt", rules), QStringLiteral("<%1>").arg(RenameRules::problem(rules)));
    }
    // What the expression does have: two groups, one of them by name.
    QVERIFY(RenameRules::problem(Rules{.find = "(a)(?<b>c)?", .replace = "$2 ${b} $1 ${1} $0 ${0} $01",
                                  .regularExpression = true})
            .isEmpty());
    // With nothing to find, nothing is replaced and nothing can be wrong with the rest.
    QVERIFY(RenameRules::problem(Rules{.replace = "$9 (", .regularExpression = true}).isEmpty());
    QCOMPARE(newName("a.txt", Rules{.replace = "$9 (", .regularExpression = true}), QStringLiteral("a.txt"));
    // The counter needs a digit.
    QCOMPARE(newName("a.txt", Rules{.counterWidth = 0}), QStringLiteral("<The counter needs at least one digit.>"));
}

void TestMultiRename::caseRules()
{
    QCOMPARE(newName("My File.TXT", Rules{.caseMode = Case::Lower}), QStringLiteral("my file.txt"));
    QCOMPARE(newName("My File.TXT", Rules{.caseMode = Case::Upper}), QStringLiteral("MY FILE.TXT"));
    QCOMPARE(newName("my holiday photos.jpg", Rules{.caseMode = Case::Title}),
             QStringLiteral("My Holiday Photos.jpg"));
    // Only the first letter of a word changes.
    QCOMPARE(newName("the CAT came back.TXT", Rules{.caseMode = Case::Title}),
             QStringLiteral("The CAT Came Back.TXT"));
    QCOMPARE(newName("1st place (2nd time)", Rules{.caseMode = Case::Title}),
             QStringLiteral("1st Place (2nd Time)"));
    QCOMPARE(newName("(my files)", Rules{.caseMode = Case::Title}), QStringLiteral("(My Files)"));
    QCOMPARE(newName("two  spaces  everywhere", Rules{.caseMode = Case::Title}),
             QStringLiteral("Two  Spaces  Everywhere"));
    QCOMPARE(newName("ÉCOLE été", Rules{.caseMode = Case::Title}), QStringLiteral("ÉCOLE Été"));
    // What has no case stays as it is.
    QCOMPARE(newName(QString::fromUtf8("한국어 FILE.txt"), Rules{.caseMode = Case::Lower}),
             QString::fromUtf8("한국어 file.txt"));
    // After the mask and the find-and-replace.
    QCOMPARE(newName("PHOTO.JPG", Rules{.mask = "[N]-[E]", .caseMode = Case::Lower}),
             QStringLiteral("photo-jpg"));
    QCOMPARE(newName("a.txt", Rules{.find = "a", .replace = "B C", .caseMode = Case::Upper}),
             QStringLiteral("B C.TXT"));
    QCOMPARE(newName("a_b c.txt", Rules{.caseMode = Case::Title, .underscoresToSpaces = true}),
             QStringLiteral("A B C.txt"));
}

void TestMultiRename::underscores()
{
    QCOMPARE(newName("my_file.txt", Rules{.underscoresToSpaces = true}), QStringLiteral("my file.txt"));
    QCOMPARE(newName("my__file.txt", Rules{.underscoresToSpaces = true}), QStringLiteral("my  file.txt"));
    QCOMPARE(newName("my_file.txt", Rules()), QStringLiteral("my_file.txt"));
    // From the mask and from the replacement too.
    QCOMPARE(newName("a.txt", Rules{.mask = "x_y_[N]", .underscoresToSpaces = true}), QStringLiteral("x y a"));
    QCOMPARE(newName("a.txt", Rules{.find = "a", .replace = "b_c", .underscoresToSpaces = true}),
             QStringLiteral("b c.txt"));
}

void TestMultiRename::namesThatCannotBeUsed()
{
    // Empty.
    QCOMPARE(newName("a.txt", Rules{.mask = ""}), QStringLiteral("<The new name would be empty.>"));
    QCOMPARE(newName("README", Rules{.mask = "[E]"}), QStringLiteral("<The new name would be empty.>"));
    QCOMPARE(newName("README", Rules{.mask = ".[E]"}), QStringLiteral("<The new name would be empty.>"));
    QCOMPARE(newName("a", Rules{.find = "a", .replace = ""}), QStringLiteral("<The new name would be empty.>"));
    QCOMPARE(newName("a.txt", Rules{.find = ".*", .replace = "", .regularExpression = true}),
             QStringLiteral("<The new name would be empty.>"));

    // What no file may be called: the name is there, with why it can't be used.
    // It is the reason FileOps::isAllowedName gives, as FileOps::rename would.
    const Result slash = RenameRules::newName(Source{"photo.jpg", {}, 0}, Rules{.mask = "[N]/x.[E]"});
    QVERIFY(!slash.ok());
    QCOMPARE(slash.name, QStringLiteral("photo/x.jpg"));
    QCOMPARE(slash.error, notAllowed("photo/x.jpg"));
    const Result dot = RenameRules::newName(Source{"photo.jpg", {}, 0}, Rules{.mask = "[N]."});
    QCOMPARE(dot.name, QStringLiteral("photo."));
    QVERIFY(!dot.ok());
    for (const Rules &rules : {Rules{.mask = "[N]x:1.[E]"}, Rules{.mask = "[N] "},
                               Rules{.mask = "con.[E]"}, Rules{.mask = "LPT1"}, Rules{.mask = ".."},
                               Rules{.mask = "[N]a\\b"}, Rules{.find = "jpg", .replace = "jp g "},
                               Rules{.mask = "[N]", .find = "o", .replace = "?"}}) {
        const Result result = RenameRules::newName(Source{"photo.jpg", {}, 0}, rules);
        QVERIFY2(!result.ok(), qPrintable(result.name));
        QVERIFY(!result.name.isEmpty());
        QVERIFY2(!result.error.isEmpty(), qPrintable(result.name));
    }
    // The same rules are fine for another file.
    QCOMPARE(newName("a.txt", Rules{.find = "o", .replace = "?"}), QStringLiteral("a.txt"));

    // What a file is called already isn't its new name's fault.
    QCOMPARE(newName("a:b.txt", Rules()), QStringLiteral("a:b.txt"));
    QCOMPARE(newName("a:b.txt", Rules{.mask = "[N]x.[E]"}), QStringLiteral("<%1>").arg(notAllowed("a:bx.txt")));
    QCOMPARE(newName("CON", Rules{.mask = "[L][N]"}), QStringLiteral("<%1>").arg(notAllowed("con")));
    QCOMPARE(newName("name", Rules{.mask = "[N]x "}), QStringLiteral("<%1>").arg(notAllowed("namex ")));
    // Only the case changing is a new name.
    QCOMPARE(newName("Case.txt", Rules{.mask = "[U][N].[E]"}), QStringLiteral("CASE.TXT"));
}

void TestMultiRename::theRulesInOrder()
{
    // The mask, then find and replace, then underscores, then the case.
    QCOMPARE(newName("a b.txt", Rules{.find = " ", .replace = "_", .underscoresToSpaces = true}),
             QStringLiteral("a b.txt"));
    QCOMPARE(newName("a-b.txt", Rules{.find = "-", .replace = "_", .caseMode = Case::Title, .underscoresToSpaces = true}),
             QStringLiteral("A B.txt"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[U][N]", .caseMode = Case::Lower}), QStringLiteral("photo"));
    QCOMPARE(newName("photo.jpg", Rules{.mask = "[L][N]", .find = "o", .replace = "O", .caseMode = Case::Title}),
             QStringLiteral("PhOtO"));
    // The order of the settings doesn't matter to the result.
    QCOMPARE(newName("a_b.txt", Rules{.caseMode = Case::Title, .underscoresToSpaces = true}),
             newName("a_b.txt", Rules{.caseMode = Case::Title, .underscoresToSpaces = true}));
}

void TestMultiRename::showsWhatWouldHappen()
{
    Fixture f;
    const QString korean = QString::fromUtf8("한국어 파일");
    std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt", "README", korean + ".jpg"});

    // Nothing is asked of the rules: every name stays, and there is nothing to start.
    QCOMPARE(window->rows().size(), 4);
    QCOMPARE(newNames(window->rows()), (QStringList{"a.txt", "b.txt", "README", korean + ".jpg"}));
    for (const MultiRenameWindow::Row &row : window->rows()) {
        QCOMPARE(row.state, State::Unchanged);
        QCOMPARE(row.path, f.folder + "/" + row.name);
    }
    QCOMPARE(window->renameCount(), 0);
    QCOMPARE(window->problemCount(), 0);
    QCOMPARE(window->statusText(), QStringLiteral("No file will be renamed, 4 unchanged"));
    QVERIFY(!window->renameButton()->isEnabled());
    QVERIFY(!window->undoButton()->isEnabled());

    // A mask with a counter: what every file would be called, from the first one on.
    window->setMask("[U][N]_[C:2].[E]");
    // From [U] on, the extension is in upper case too.
    QCOMPARE(newNames(window->rows()), (QStringList{"A_01.TXT", "B_02.TXT", "README_03", korean + "_04.JPG"}));
    QCOMPARE(window->renameCount(), 4);
    QCOMPARE(window->problemCount(), 0);
    QCOMPARE(window->statusText(), QStringLiteral("All 4 files will be renamed"));
    QVERIFY(window->renameButton()->isEnabled());

    // The table shows what the rows say.
    const QAbstractItemModel &model = *window->table()->model();
    QCOMPARE(model.rowCount(), 4);
    QCOMPARE(model.headerData(1, Qt::Horizontal).toString(), QStringLiteral("New name"));
    QCOMPARE(model.headerData(3, Qt::Horizontal).toString(), QStringLiteral("Status"));
    QCOMPARE(model.index(2, 0).data().toString(), QStringLiteral("README"));
    QCOMPARE(model.index(2, 1).data().toString(), QStringLiteral("README_03"));
    QCOMPARE(model.index(2, 3).data().toString(), QString());  // nothing the matter with it
    // The folder, with the home folder as ~.
    const QString home = QDir::homePath();
    const QString shown = f.folder.startsWith(home + '/') ? "~" + f.folder.mid(home.size()) : f.folder;
    QCOMPARE(model.index(2, 2).data().toString(), QDir::toNativeSeparators(shown));

    // Only where something is found does anything change; whatever its case.
    window->setMask("[N].[E]");
    window->setFind("a");
    window->setReplace("o");
    QCOMPARE(newNames(window->rows()), (QStringList{"o.txt", "b.txt", "REoDME", korean + ".jpg"}));
    QCOMPARE(window->statusText(), QStringLiteral("2 of 4 files will be renamed, 2 unchanged"));

    // What the fields say is what the rules are.
    Settings settings = window->settings();
    QCOMPARE(settings.mask, QStringLiteral("[N].[E]"));
    QCOMPARE(settings.find, QStringLiteral("a"));
    QCOMPARE(settings.replace, QStringLiteral("o"));
    QVERIFY(!settings.regularExpression);
    QCOMPARE(settings.caseMode, Case::Unchanged);
    QCOMPARE(settings.counterStart, 1);
    QCOMPARE(settings.counterWidth, 1);
    QVERIFY(!settings.underscoresToSpaces);
    window->setCase(Case::Upper);
    window->setCounterStart(7);
    window->setCounterWidth(3);
    window->setRegularExpression(true);
    window->setUnderscoresToSpaces(true);
    settings = window->settings();
    QCOMPARE(settings.caseMode, Case::Upper);
    QCOMPARE(settings.counterStart, 7);
    QCOMPARE(settings.counterWidth, 3);
    QVERIFY(settings.regularExpression);
    QVERIFY(settings.underscoresToSpaces);
    // All of it at once: the mask, a found "a" replaced, underscores, the case, counting from 7.
    window->setMask("[N]_[C].[E]");
    QCOMPARE(newNames(window->rows()),
             (QStringList{"O 007.TXT", "B 008.TXT", "README 009", korean + QString::fromUtf8(" 010.JPG")}));
}

void TestMultiRename::theDateOfTheFile()
{
    Fixture f;
    const QString path = f.folder + "/a.txt";
    writeFile(path, "a");
    setModified(path, kDate);
    std::unique_ptr<MultiRenameWindow> window = f.open(QStringList{path});
    window->setMask("[Y][M][D]_[h][m][s].[E]");
    QCOMPARE(newNames(window->rows()), QStringList{QStringLiteral("20240307_090531.txt")});
    QCOMPARE(window->statusText(), QStringLiteral("The file will be renamed"));
}

void TestMultiRename::thePreviewFollowsTheTyping()
{
    Fixture f;
    std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt"});
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    // The mask is selected when the window opens: typing makes a new one.
    QCOMPARE(QApplication::focusWidget(), window->maskField());
    QCOMPARE(window->maskField()->selectedText(), QStringLiteral("[N].[E]"));

    QTest::keyClicks(window->maskField(), "[N]_copy.[E]");
    QTRY_COMPARE(window->renameCount(), 2);
    QCOMPARE(newNames(window->rows()), (QStringList{"a_copy.txt", "b_copy.txt"}));
    // Typing changes nothing on disk.
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt"}));

    // What is found is typed first, then what comes instead.
    window->findField()->setFocus();
    QTest::keyClicks(window->findField(), "_copy");
    QTRY_COMPARE(window->renameCount(), 0);
    window->replaceField()->setFocus();
    QTest::keyClicks(window->replaceField(), " Copy");
    QTRY_VERIFY(window->renameCount() == 2);
    QCOMPARE(newNames(window->rows()), (QStringList{"a Copy.txt", "b Copy.txt"}));

    // A button under the mask puts its placeholder where the mask's cursor is, and leaves the focus there.
    window->maskField()->setFocus();
    QToolButton *counter = nullptr;
    for (QToolButton *button : window->findChildren<QToolButton *>()) {
        if (button->text() == QLatin1String("[C] Counter")) {
            counter = button;
        }
    }
    QVERIFY(counter);
    QTest::mouseClick(counter, Qt::LeftButton);
    QCOMPARE(window->maskField()->text(), QStringLiteral("[N]_copy.[E][C]"));
    QCOMPARE(QApplication::focusWidget(), window->maskField());
    QTRY_COMPARE(newNames(window->rows()), (QStringList{"a Copy.txt1", "b Copy.txt2"}));

    // The field for "find" emptied: the underscore of the mask is back, and then a box turns it into a space.
    window->findField()->setFocus();
    QTest::keyClick(window->findField(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window->findField(), Qt::Key_Delete);
    QTRY_COMPARE(newNames(window->rows()), (QStringList{"a_copy.txt1", "b_copy.txt2"}));
    QTest::mouseClick(window->underscoresBox(), Qt::LeftButton);
    QTRY_COMPARE(newNames(window->rows()), (QStringList{"a copy.txt1", "b copy.txt2"}));

    // Enter in a field is not "Rename": the files are as they were.
    QTest::keyClick(window->maskField(), Qt::Key_Return);
    QTest::keyClick(window->maskField(), Qt::Key_Enter);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt"}));
    QVERIFY(!window->canUndo());

    // Escape closes the window, which then deletes itself.
    QPointer<MultiRenameWindow> itself = window.release();
    itself->setAttribute(Qt::WA_DeleteOnClose);
    QTest::keyClick(itself->maskField(), Qt::Key_Escape);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(itself.isNull());
}

void TestMultiRename::theSameNewNameForTwoFiles()
{
    Fixture f;
    std::unique_ptr<MultiRenameWindow> window = f.window({"IMG_0001.jpg", "IMG_0002.jpg", "photo.jpg"});
    window->setMask("same.[E]");
    // All three would be "same.jpg": each row says which other file is in its way, and none is renamed.
    QCOMPARE(newNames(window->rows()), (QStringList{"same.jpg", "same.jpg", "same.jpg"}));
    QCOMPARE(window->renameCount(), 0);
    QCOMPARE(window->problemCount(), 3);
    QCOMPARE(window->statusText(), QStringLiteral("No file will be renamed, 3 problems"));
    QList<MultiRenameWindow::Row> rows = window->rows();
    QCOMPARE(rows.at(0).message, QStringLiteral("The same new name as IMG_0002.jpg."));
    QCOMPARE(rows.at(1).message, QStringLiteral("The same new name as IMG_0001.jpg."));
    QCOMPARE(rows.at(2).message, QStringLiteral("The same new name as IMG_0001.jpg."));
    QVERIFY(!window->renameButton()->isEnabled());
    QCOMPARE(window->renameAll(), 0);
    QCOMPARE(folderHolds(f.folder), (QStringList{"IMG_0001.jpg", "IMG_0002.jpg", "photo.jpg"}));

    // The two that collide are left alone; the third gets a name of its own.
    Fixture g;
    std::unique_ptr<MultiRenameWindow> other = g.window({"1a.txt", "2a.txt", "3b.txt"});
    other->setFind("^\\d");
    other->setReplace("");
    other->setRegularExpression(true);
    QCOMPARE(newNames(other->rows()), (QStringList{"a.txt", "a.txt", "b.txt"}));
    QCOMPARE(other->renameCount(), 1);
    QCOMPARE(other->problemCount(), 2);
    QCOMPARE(other->renameAll(), 1);
    QCOMPARE(folderHolds(g.folder), (QStringList{"1a.txt", "2a.txt", "b.txt"}));
}

void TestMultiRename::aNameThatIsTaken()
{
    // A file that is there already.
    {
        Fixture f;
        std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt"});
        window->setMask("b.[E]");
        QCOMPARE(newNames(window->rows()), (QStringList{"b.txt", "b.txt"}));
        QCOMPARE(window->rows().at(0).state, State::Problem);
        QVERIFY(!window->rows().at(0).message.isEmpty());
        QCOMPARE(window->rows().at(1).state, State::Unchanged);
        QCOMPARE(window->statusText(), QStringLiteral("No file will be renamed, 1 problem, 1 unchanged"));
        QCOMPARE(window->renameAll(), 0);
        QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt"}));
    }
    // A name that differs only in case is the same name.
    {
        Fixture f;
        std::unique_ptr<MultiRenameWindow> window = f.window({"A.txt", "B.txt"});
        window->setFind("A");
        window->setReplace("b");
        QCOMPARE(window->rows().at(0).state, State::Problem);
        QCOMPARE(window->rows().at(1).state, State::Unchanged);
        QCOMPARE(window->renameAll(), 0);
        QCOMPARE(folderHolds(f.folder), (QStringList{"A.txt", "B.txt"}));
    }
    // Changing only the case of a file's own name is a rename like any other.
    {
        Fixture f;
        std::unique_ptr<MultiRenameWindow> window = f.window({"Case.txt"});
        window->setMask("[U][N].[E]");
        QCOMPARE(window->renameCount(), 1);
        QCOMPARE(window->renameAll(), 1);
        QCOMPARE(folderHolds(f.folder), QStringList{"CASE.TXT"});
        QCOMPARE(window->undo(), 1);
        QCOMPARE(folderHolds(f.folder), QStringList{"Case.txt"});
    }
    // Of three files, the one whose new name is taken stays as it is.
    {
        Fixture f;
        std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt", "1.txt"});
        window->setMask("[C].[E]");
        QCOMPARE(newNames(window->rows()), (QStringList{"1.txt", "2.txt", "3.txt"}));
        QCOMPARE(window->rows().at(0).state, State::Problem);  // 1.txt is there, and stays
        QCOMPARE(window->renameCount(), 2);
        QCOMPARE(window->renameAll(), 2);
        QCOMPARE(folderHolds(f.folder), (QStringList{"2.txt", "3.txt", "a.txt"}));
        QCOMPARE(window->rows().at(0).path, f.folder + "/a.txt");
    }
}

void TestMultiRename::whatOnlySomeFilesCanUse()
{
    Fixture f;
    std::unique_ptr<MultiRenameWindow> window = f.window({"photo.jpg", "a.txt", "x.txt"});
    window->setFind("x.txt");
    window->setReplace("?.txt");
    // Only the file that gets a name that can't be used is a problem, with the reason FileOps gives.
    QCOMPARE(window->rows().at(0).state, State::Unchanged);
    QCOMPARE(window->rows().at(1).state, State::Unchanged);
    QCOMPARE(window->rows().at(2).state, State::Problem);
    QCOMPARE(window->rows().at(2).newName, QStringLiteral("?.txt"));
    QCOMPARE(window->rows().at(2).message, notAllowed("?.txt"));
    QCOMPARE(window->statusText(), QStringLiteral("No file will be renamed, 1 problem, 2 unchanged"));

    // What makes no name at all is a problem of that one file; the other goes on.
    Fixture g;
    std::unique_ptr<MultiRenameWindow> other = g.window({"README", "a.txt"});
    other->setMask("[E]");
    QCOMPARE(other->rows().at(0).message, QStringLiteral("The new name would be empty."));
    QCOMPARE(other->renameCount(), 1);
    QCOMPARE(other->renameAll(), 1);
    QCOMPARE(folderHolds(g.folder), (QStringList{"README", "txt"}));

    // A file that is called what a new file may not be called is left at that, not called a problem.
    const QString odd = f.folder + "/a:b.txt";
    if (writeFile(odd, "odd")) {
        std::unique_ptr<MultiRenameWindow> window = f.open(QStringList{odd});
        QCOMPARE(window->rows().at(0).state, State::Unchanged);
        QCOMPARE(window->problemCount(), 0);
    }

    // What is wrong with the rules is said where everyone looks, and in every row; nothing is renamed.
    window->setFind("");
    window->setMask("[X]");
    QCOMPARE(window->statusText(), QStringLiteral("Unknown placeholder [X]."));
    QCOMPARE(window->problemCount(), 3);
    for (const MultiRenameWindow::Row &row : window->rows()) {
        QCOMPARE(row.state, State::Problem);
        QCOMPARE(row.message, window->statusText());
    }
    window->setMask("[N]");
    window->setRegularExpression(true);
    window->setFind("(");
    QVERIFY(!RenameRules::problem(window->settings()).isEmpty());
    QCOMPARE(window->statusText(), RenameRules::problem(window->settings()));
    QCOMPARE(window->renameAll(), 0);
    QCOMPARE(folderHolds(f.folder).size(), 3 + (QFileInfo::exists(odd) ? 1 : 0));
}

void TestMultiRename::renamingAndUndo()
{
    Fixture f;
    std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt", "README"});
    const QStringList paths = window->files();
    QSignalSpy changed(window.get(), &MultiRenameWindow::filesChanged);
    window->setMask("[C]_[N].[E]");
    QCOMPARE(newNames(window->rows()), (QStringList{"1_a.txt", "2_b.txt", "3_README"}));
    // Nothing is on disk until Rename.
    QCOMPARE(folderHolds(f.folder), (QStringList{"README", "a.txt", "b.txt"}));

    QCOMPARE(window->renameAll(), 3);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(folderHolds(f.folder), (QStringList{"1_a.txt", "2_b.txt", "3_README"}));

    // The table says what happened: the name before, the name after, and that it was renamed.
    QList<MultiRenameWindow::Row> rows = window->rows();
    QCOMPARE(rows.at(0).name, QStringLiteral("a.txt"));
    QCOMPARE(rows.at(0).newName, QStringLiteral("1_a.txt"));
    QCOMPARE(rows.at(0).path, f.folder + "/1_a.txt");
    for (const MultiRenameWindow::Row &row : rows) {
        QCOMPARE(row.state, State::Renamed);
        QVERIFY(row.message.isEmpty());
    }
    QCOMPARE(window->statusText(), QStringLiteral("3 files renamed"));
    QCOMPARE(window->table()->model()->index(0, 3).data().toString(), QStringLiteral("Renamed"));
    QVERIFY(!window->renameButton()->isEnabled());
    QCOMPARE(window->files(),
             (QStringList{f.folder + "/1_a.txt", f.folder + "/2_b.txt", f.folder + "/3_README"}));

    // Undo gives the batch its names back.
    QVERIFY(window->canUndo());
    QVERIFY(window->undoButton()->isEnabled());
    QCOMPARE(window->undo(), 3);
    QCOMPARE(changed.size(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"README", "a.txt", "b.txt"}));
    QCOMPARE(window->files(), paths);
    rows = window->rows();
    QCOMPARE(rows.at(0).name, QStringLiteral("1_a.txt"));
    QCOMPARE(rows.at(0).newName, QStringLiteral("a.txt"));
    for (const MultiRenameWindow::Row &row : rows) {
        QCOMPARE(row.state, State::Restored);
    }
    QCOMPARE(window->statusText(), QStringLiteral("3 files given back their names"));
    QVERIFY(!window->canUndo());
    QVERIFY(!window->undoButton()->isEnabled());
    QCOMPARE(window->undo(), 0);
    QCOMPARE(changed.size(), 2);

    // All of it without one question or message.
    QVERIFY(f.dialogs.questions.isEmpty());
    QVERIFY(f.dialogs.told.isEmpty());
    QCOMPARE(f.dialogs.textsAsked, 0);
}

void TestMultiRename::undoStepByStep()
{
    Fixture f;
    std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt"});
    window->setMask("[N]_1.[E]");
    QCOMPARE(window->renameAll(), 2);

    // The rules go on from the names the files have now; that is a new preview, not the old report.
    window->setMask("[N]_2.[E]");
    QCOMPARE(newNames(window->rows()), (QStringList{"a_1_2.txt", "b_1_2.txt"}));
    QCOMPARE(window->rows().at(0).name, QStringLiteral("a_1.txt"));
    QCOMPARE(window->statusText(), QStringLiteral("All 2 files will be renamed"));
    QCOMPARE(window->renameAll(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a_1_2.txt", "b_1_2.txt"}));

    // Each rename is a batch of its own, the last one undone first.
    QCOMPARE(window->undo(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a_1.txt", "b_1.txt"}));
    QVERIFY(window->canUndo());
    QCOMPARE(window->undo(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt"}));
    QVERIFY(!window->canUndo());
}

// A file that has gone, and a name that is taken, do not stop the others:
// each row says what became of its file, and Undo knows what was renamed.
void TestMultiRename::aFileThatFailsDoesNotStopTheOthers()
{
    Fixture f;
    const QStringList files = makeFiles(f.folder, {"a.txt", "b.txt", "c.txt"});
    const QString inOther = f.other + "/d.txt";
    writeFile(inOther, "d");
    std::unique_ptr<MultiRenameWindow> window = f.open(files + QStringList{inOther});
    window->setMask("[N]_new.[E]");
    QCOMPARE(window->renameCount(), 4);

    // After the preview, one file is gone and the name another one would get is taken.
    QVERIFY(QFile::remove(f.folder + "/b.txt"));
    writeFile(f.folder + "/c_new.txt", "in the way");
    QCOMPARE(window->renameAll(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a_new.txt", "c.txt", "c_new.txt"}));
    QCOMPARE(folderHolds(f.other), QStringList{"d_new.txt"});

    // Every row says what became of its file.
    QList<MultiRenameWindow::Row> rows = window->rows();
    QCOMPARE(rows.at(0).state, State::Renamed);
    QCOMPARE(rows.at(1).state, State::Failed);
    QVERIFY(!rows.at(1).message.isEmpty());
    QCOMPARE(rows.at(1).name, QStringLiteral("b.txt"));
    QCOMPARE(rows.at(1).path, f.folder + "/b.txt");
    QCOMPARE(rows.at(2).state, State::Failed);
    QVERIFY(!rows.at(2).message.isEmpty());
    QCOMPARE(rows.at(2).path, f.folder + "/c.txt");
    QCOMPARE(rows.at(3).state, State::Renamed);
    QCOMPARE(window->statusText(), QStringLiteral("2 files renamed, 2 files could not be"));
    const QString said = window->table()->model()->index(1, 3).data().toString();
    QVERIFY2(said.startsWith(QStringLiteral("Could not be renamed: ")), qPrintable(said));

    // Undo gives back what was renamed; the two that failed were never touched.
    QCOMPARE(window->undo(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "c.txt", "c_new.txt"}));
    QCOMPARE(folderHolds(f.other), QStringList{"d.txt"});
    rows = window->rows();
    QCOMPARE(rows.at(0).state, State::Restored);
    QCOMPARE(rows.at(3).state, State::Restored);
    QCOMPARE(rows.at(1).state, State::Problem);  // gone
    QCOMPARE(rows.at(2).state, State::Problem);  // name taken
    QVERIFY(!rows.at(1).message.isEmpty());
    QVERIFY(!rows.at(2).message.isEmpty());
    QCOMPARE(window->statusText(), QStringLiteral("2 files given back their names"));
    QVERIFY(!window->canUndo());
}

void TestMultiRename::anUndoThatFailsCanBeTriedAgain()
{
    Fixture f;
    std::unique_ptr<MultiRenameWindow> window = f.window({"a.txt", "b.txt"});
    window->setMask("[U][N]");
    QCOMPARE(window->renameAll(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"A", "B"}));

    // Before Undo, the old name of one of them is taken by another file.
    writeFile(f.folder + "/a.txt", "in the way");
    QCOMPARE(window->undo(), 1);
    QCOMPARE(folderHolds(f.folder), (QStringList{"A", "a.txt", "b.txt"}));
    QList<MultiRenameWindow::Row> rows = window->rows();
    QCOMPARE(rowFor(rows, "a.txt").state, State::RestoreFailed);
    QVERIFY(!rowFor(rows, "a.txt").message.isEmpty());
    QCOMPARE(rowFor(rows, "b.txt").state, State::Restored);
    QCOMPARE(window->statusText(), QStringLiteral("1 file given back their names, 1 file could not be"));

    // What could not be given back stays to be tried again, and is once the way is free.
    QVERIFY(window->canUndo());
    QVERIFY(QFile::remove(f.folder + "/a.txt"));
    QCOMPARE(window->undo(), 1);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt"}));
    QVERIFY(!window->canUndo());

    // A file that was renamed and then deleted can't be given back either.
    Fixture g;
    std::unique_ptr<MultiRenameWindow> other = g.window({"a.txt"});
    other->setMask("[U][N]");
    QCOMPARE(other->renameAll(), 1);
    QVERIFY(QFile::remove(g.folder + "/A"));
    QCOMPARE(other->undo(), 0);
    QCOMPARE(other->rows().at(0).state, State::RestoreFailed);
    QVERIFY(!other->rows().at(0).message.isEmpty());
    QVERIFY(other->canUndo());
}

void TestMultiRename::filesThatAreGoneAndFilesSetAgain()
{
    Fixture f;
    const QStringList files = makeFiles(f.folder, {"a.txt", "b.txt"});
    std::unique_ptr<MultiRenameWindow> window =
            f.open({files.at(0), files.at(0), files.at(1), f.folder + "/gone.txt"});
    // A file that is listed twice is one file; one that isn't there is a problem, and no more than that.
    const QString gone = f.folder + "/gone.txt";
    QCOMPARE(window->files(), files + QStringList{gone});
    QCOMPARE(window->problemCount(), 1);
    QCOMPARE(window->rows().at(2).state, State::Problem);
    QCOMPARE(window->statusText(), QStringLiteral("No file will be renamed, 1 problem, 2 unchanged"));
    window->setMask("[U][N]");
    QCOMPARE(window->renameCount(), 2);
    QCOMPARE(window->renameAll(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"A", "B"}));

    // Other files are set: the table is about them, and Undo still knows the batch before.
    const QString c = f.folder + "/c.txt";
    writeFile(c, "c");
    window->setFiles(QStringList{c});
    QCOMPARE(window->files(), QStringList{c});
    QCOMPARE(window->rows().size(), 1);
    QCOMPARE(window->statusText(), QStringLiteral("No file will be renamed, 1 unchanged"));
    QVERIFY(window->canUndo());
    QCOMPARE(window->undo(), 2);
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt", "c.txt"}));

    // The files that got their names back are in the table again, with what happened to them.
    QCOMPARE(window->rows().size(), 3);
    QCOMPARE(window->files().size(), 3);
    for (const QString &name : {QString("a.txt"), QString("b.txt")}) {
        const MultiRenameWindow::Row row = rowFor(window->rows(), name);
        QCOMPARE(row.state, State::Restored);
        // The name the file had while it was renamed.
        QCOMPARE(row.name, name.left(1).toUpper());
        QCOMPARE(row.path, f.folder + "/" + name);
    }
    QCOMPARE(window->statusText(), QStringLiteral("2 files given back their names"));
    QVERIFY(!window->canUndo());
}

void TestMultiRename::openedForASelection()
{
    Fixture f;
    const QStringList files = makeFiles(f.folder, {"a.txt", "b.txt"});
    QPointer<MultiRenameWindow> window = MultiRenameWindow::open(files, f.dialogs);
    QVERIFY(window);
    QVERIFY(window->isVisible());
    QVERIFY(!window->isModal());
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(window->windowTitle().contains(QStringLiteral("Multi-rename")));
    QCOMPARE(window->rows().size(), 2);
    // The window opens at a size the list can be worked in; how big that
    // is exactly, the system decides, which may clamp what was asked for.
    QVERIFY(window->size().width() >= 640);
    QVERIFY(window->size().height() >= 400);

    // Closed: gone, and nothing was renamed.
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.isNull());
    QCOMPARE(folderHolds(f.folder), (QStringList{"a.txt", "b.txt"}));
}

QTEST_MAIN(TestMultiRename)
#include "multirename.moc"
