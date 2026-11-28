#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPixmap>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <memory>

#include "helpers.h"
#include "panels/FileProxyModel.h"
#include "panels/FileView.h"

namespace {

constexpr Qt::DropActions kCopyOrMove = Qt::CopyAction | Qt::MoveAction;

// Keeps the drag instead of running it, so that the test can see what the
// view would have dragged.
class RecordingView : public FileView {
public:
    using FileView::FileView;

    int drags = 0;
    QList<QUrl> draggedUrls;
    QByteArray draggedSource;

protected:
    void performDrag(QMimeData *mime, const QPixmap &image, const QPoint &) override
    {
        ++drags;
        draggedUrls = mime->urls();
        draggedSource = mime->data(dragSourceFormat);
        QVERIFY(image.size().width() > 0);
        delete mime;
    }
};

// A folder with folder-a, folder-b and file00.txt … file09.txt, shown in a
// window of their own. The view is a RecordingView.
struct Fixture {
    Fixture()
        : dir(root.path())
    {
        created = makeFolder(dir + "/folder-a") && makeFolder(dir + "/folder-b")
                 && makeFolder(dir + "/folder-a/inner");
        for (int i = 0; i < 10; ++i) {
            created = writeFile(dir + QString("/file%1.txt").arg(i, 2, 10, QLatin1Char('0')), "x")
                      && created;
        }
        proxy.setSourceModel(&model);
        view.setFileModel(&proxy);
        if (created) {
            model.setPath(dir);
        }
        listed = created && waitListed(model);

        view.resize(660, 470);
        view.show();
        listed = QTest::qWaitForWindowExposed(&view) && listed;
        view.setFocus(Qt::OtherFocusReason);
        QTest::qWait(20);
    }

    int rowOf(const QString &name) const { return rowOfName(proxy, name); }
    QModelIndex index(const QString &name) const { return proxy.index(rowOf(name), FileModel::NameColumn); }
    QPoint rowCenter(int row) const
    {
        return QPoint(view.columnViewportPosition(FileModel::NameColumn) + 24,
                      view.rowViewportPosition(row) + view.rowHeight(row) / 2);
    }
    QPoint pointOf(const QString &name) const { return rowCenter(rowOf(name)); }
    // Below the last row, where the table is empty.
    QPoint emptyPoint() const { return QPoint(view.viewport()->width() / 2, view.viewport()->height() - 12); }

    // Names of the selected rows, as the view reports them.
    QStringList selected() const
    {
        QStringList result;
        for (const QString &path : view.selectedPaths()) {
            result.append(QFileInfo(path).fileName());
        }
        return result;
    }
    QString currentName() const
    {
        return view.currentIndex().data().toString().normalized(QString::NormalizationForm_C);
    }
    void click(const QString &name, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::mouseClick(view.viewport(), Qt::LeftButton, modifiers, pointOf(name));
    }

    QTemporaryDir root;
    QString dir;
    FileModel model;
    FileProxyModel proxy;
    RecordingView view;
    bool created = false;
    bool listed = false;
};

// Press, move in a straight line, release.
void dragMouse(QWidget *widget, const QPoint &from, const QPoint &to, int steps = 6)
{
    QTest::mousePress(widget, Qt::LeftButton, Qt::NoModifier, from);
    for (int i = 1; i <= steps; ++i) {
        QTest::mouseMove(widget, from + (to - from) * i / steps);
    }
    QTest::mouseRelease(widget, Qt::LeftButton, Qt::NoModifier, to);
}

// Delivers a drop to the view's viewport; whether it was accepted. Qt only
// delivers a drop to a view that took the drag when it entered, so that is
// sent first.
bool dropAt(Fixture &f, const QMimeData &mime, const QPoint &position,
            Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QDragEnterEvent enter(position, kCopyOrMove, &mime, Qt::NoButton, modifiers);
    QCoreApplication::sendEvent(f.view.viewport(), &enter);
    QDropEvent event(QPointF(position), kCopyOrMove, &mime, Qt::NoButton, modifiers);
    QCoreApplication::sendEvent(f.view.viewport(), &event);
    return event.isAccepted();
}

// Asks for the context menu at `position`, as a right-click does. (The window
// layer turns a right-click into this event; the test harness skips it.)
void rightClick(Fixture &f, const QPoint &position)
{
    QContextMenuEvent menu(QContextMenuEvent::Mouse, position,
                           f.view.viewport()->mapToGlobal(position));
    QCoreApplication::sendEvent(f.view.viewport(), &menu);
}

// Arguments of a FileView::dropped signal.
struct Drop {
    QList<QUrl> urls;
    QString target;
    Qt::DropAction action;
    bool ask;
};

Drop dropFrom(const QVariantList &arguments)
{
    return {arguments.at(0).value<QList<QUrl>>(), arguments.at(1).toString(),
            arguments.at(2).value<Qt::DropAction>(), arguments.at(3).toBool()};
}

QStringList fileNames(const QList<QUrl> &urls)
{
    QStringList names;
    for (const QUrl &url : urls) {
        names.append(QFileInfo(url.toLocalFile()).fileName());
    }
    return names;
}

}  // namespace

class TestFileView : public QObject {
    Q_OBJECT

private slots:
    void clickSelectsShiftExtendsCtrlToggles();
    void pressAndDragSweepsARange();
    void dragStartsOnlyFromSelectedRows();
    void rubberBandSelects();
    void doubleClickAndEnterOpen();
    void insertAndSpaceMarkAndMoveDown();
    void typingOpensTheQuickSearchAndJumps();
    void theQuickSearchTakesWildcardsAndEscapeGoesBack();
    void arrowsSelectOneRowShiftExtends();
    void rightClickSelectsRowForMenu();
    void dropLandsInFolderUnderPointer();
    void dropChoosesCopyOrMove();
    void impossibleDropsAreRejected();
    void dragOverHighlightsFolderRow();
    void narrowPanesDropColumns();
};

void TestFileView::clickSelectsShiftExtendsCtrlToggles()
{
    Fixture f;
    QVERIFY(f.listed);
    QCOMPARE(f.proxy.rowCount(), 12);
    QSignalSpy activated(&f.view, &FileView::activated);

    f.click("file03.txt");
    QCOMPARE(f.selected(), QStringList({"file03.txt"}));
    QCOMPARE(f.currentName(), QString("file03.txt"));

    f.click("file06.txt", Qt::ShiftModifier);
    QCOMPARE(f.selected(), QStringList({"file03.txt", "file04.txt", "file05.txt", "file06.txt"}));

    f.click("file08.txt", Qt::ControlModifier);
    QCOMPARE(f.selected(), QStringList({"file03.txt", "file04.txt", "file05.txt", "file06.txt",
                                        "file08.txt"}));

    f.click("file04.txt", Qt::ControlModifier);  // toggles it off again
    QCOMPARE(f.selected(), QStringList({"file03.txt", "file05.txt", "file06.txt", "file08.txt"}));
    QCOMPARE(f.view.selectedPaths().first(), f.dir + "/file03.txt");  // top to bottom

    // A plain click on one of several selected rows keeps just that row.
    f.click("file06.txt");
    QCOMPARE(f.selected(), QStringList({"file06.txt"}));
    QCOMPARE(f.currentName(), QString("file06.txt"));

    // A click on empty space clears the selection.
    QTest::mouseClick(f.view.viewport(), Qt::LeftButton, Qt::NoModifier, f.emptyPoint());
    QCOMPARE(f.selected(), QStringList());
    QVERIFY(activated.count() >= 6);
}

void TestFileView::pressAndDragSweepsARange()
{
    Fixture f;
    QVERIFY(f.listed);

    // Press on an unselected row, move down: everything in between is selected.
    dragMouse(f.view.viewport(), f.pointOf("file02.txt"), f.pointOf("file06.txt"));
    QCOMPARE(f.selected(), QStringList({"file02.txt", "file03.txt", "file04.txt", "file05.txt",
                                        "file06.txt"}));
    QCOMPARE(f.currentName(), QString("file06.txt"));

    // Upwards, and replacing what was selected before.
    dragMouse(f.view.viewport(), f.pointOf("file08.txt"), f.pointOf("file05.txt"));
    QCOMPARE(f.selected(), QStringList({"file05.txt", "file06.txt", "file07.txt", "file08.txt"}));
    QCOMPARE(f.currentName(), QString("file05.txt"));

    // Moving past the last row keeps the whole range up to the pointer.
    dragMouse(f.view.viewport(), f.pointOf("file00.txt"), f.emptyPoint());
    QCOMPARE(f.selected(), QStringList({"file00.txt", "file01.txt", "file02.txt", "file03.txt",
                                        "file04.txt", "file05.txt", "file06.txt", "file07.txt",
                                        "file08.txt", "file09.txt"}));
    QCOMPARE(f.view.drags, 0);
}

void TestFileView::dragStartsOnlyFromSelectedRows()
{
    Fixture f;
    QVERIFY(f.listed);
    QSignalSpy dragStarted(&f.view, &FileView::dragStarted);
    const int rowHeight = f.view.rowHeight(f.rowOf("file03.txt"));
    QVERIFY(rowHeight > 10);

    // Press on file03, which is not selected: moving selects instead of dragging.
    dragMouse(f.view.viewport(), f.pointOf("file03.txt"),
              f.pointOf("file03.txt") + QPoint(0, 2 * rowHeight));
    QCOMPARE(f.view.drags, 0);
    QCOMPARE(f.selected(), QStringList({"file03.txt", "file04.txt", "file05.txt"}));

    // Press on file04, which is selected now: moving drags the whole selection.
    const QPoint moved = f.pointOf("file04.txt") + QPoint(0, rowHeight);
    QTest::mousePress(f.view.viewport(), Qt::LeftButton, Qt::NoModifier, f.pointOf("file04.txt"));
    QTest::mouseMove(f.view.viewport(), moved);
    QTest::mouseRelease(f.view.viewport(), Qt::LeftButton, Qt::NoModifier, moved);
    QCOMPARE(f.view.drags, 1);
    QCOMPARE(dragStarted.count(), 1);
    QCOMPARE(fileNames(f.view.draggedUrls), QStringList({"file03.txt", "file04.txt", "file05.txt"}));
    QCOMPARE(f.view.draggedSource, f.dir.toUtf8());
    QVERIFY(f.view.isDragOutEnabled());

    // Switched off, the same gesture selects instead of dragging.
    f.view.setDragOutEnabled(false);
    QVERIFY(!f.view.isDragOutEnabled());
    dragMouse(f.view.viewport(), f.pointOf("file05.txt"),
              f.pointOf("file05.txt") + QPoint(0, -2 * rowHeight));
    QCOMPARE(f.view.drags, 1);
    QCOMPARE(dragStarted.count(), 1);
}

void TestFileView::rubberBandSelects()
{
    Fixture f;
    QVERIFY(f.listed);
    QCOMPARE(f.selected(), QStringList());
    QVERIFY(f.emptyPoint().y() > f.pointOf("file09.txt").y());

    // From empty space up to the middle of file07: file07, file08 and file09.
    dragMouse(f.view.viewport(), f.emptyPoint(), f.pointOf("file07.txt"));
    QCOMPARE(f.selected(), QStringList({"file07.txt", "file08.txt", "file09.txt"}));

    // From empty space past the top: everything.
    dragMouse(f.view.viewport(), f.emptyPoint(), QPoint(f.emptyPoint().x(), 0));
    QCOMPARE(f.selected().size(), 12);

    // Back into empty space only: nothing.
    dragMouse(f.view.viewport(), f.emptyPoint(), f.emptyPoint() - QPoint(0, 4));
    QCOMPARE(f.selected(), QStringList());

    // Replaces an existing selection instead of adding to it.
    f.click("file01.txt");
    dragMouse(f.view.viewport(), f.emptyPoint(), f.pointOf("file08.txt"));
    QCOMPARE(f.selected(), QStringList({"file08.txt", "file09.txt"}));
    QCOMPARE(f.view.drags, 0);
}

void TestFileView::doubleClickAndEnterOpen()
{
    Fixture f;
    QVERIFY(f.listed);
    QSignalSpy opened(&f.view, &FileView::openRequested);
    QSignalSpy up(&f.view, &FileView::upRequested);

    QTest::mouseDClick(f.view.viewport(), Qt::LeftButton, Qt::NoModifier, f.pointOf("folder-b"));
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toModelIndex().row(), f.rowOf("folder-b"));
    // Opening is not renaming: no editor may appear.
    QVERIFY(f.view.indexWidget(f.index("folder-b")) == nullptr);

    f.view.setCurrentIndex(f.index("file05.txt"));
    QTest::keyClick(&f.view, Qt::Key_Return);
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toModelIndex().row(), f.rowOf("file05.txt"));
    QTest::keyClick(&f.view, Qt::Key_Enter);
    QCOMPARE(opened.count(), 3);

    QTest::keyClick(&f.view, Qt::Key_Backspace);
    QCOMPARE(up.count(), 1);
    QCOMPARE(opened.count(), 3);
}

void TestFileView::insertAndSpaceMarkAndMoveDown()
{
    Fixture f;
    QVERIFY(f.listed);
    // Moving the cursor marks the row it lands on; the arrow keys do the same.
    f.view.setCurrentIndex(f.index("file00.txt"));
    QCOMPARE(f.selected(), QStringList({"file00.txt"}));

    // The only marked row is where the cursor is: it stays marked.
    QTest::keyClick(&f.view, Qt::Key_Insert);
    QCOMPARE(f.selected(), QStringList({"file00.txt"}));
    QCOMPARE(f.currentName(), QString("file01.txt"));

    // The row under the cursor gets marked, and the cursor moves on.
    QTest::keyClick(&f.view, Qt::Key_Space);
    QCOMPARE(f.selected(), QStringList({"file00.txt", "file01.txt"}));
    QCOMPARE(f.currentName(), QString("file02.txt"));
    QTest::keyClick(&f.view, Qt::Key_Space);
    QCOMPARE(f.selected(), QStringList({"file00.txt", "file01.txt", "file02.txt"}));
    QCOMPARE(f.currentName(), QString("file03.txt"));

    // Ctrl-arrows move the cursor and leave the marks alone. Pressing again
    // on a marked row unmarks it.
    QTest::keyClick(&f.view, Qt::Key_Up, Qt::ControlModifier);
    QTest::keyClick(&f.view, Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(f.currentName(), QString("file01.txt"));
    QCOMPARE(f.selected(), QStringList({"file00.txt", "file01.txt", "file02.txt"}));
    QTest::keyClick(&f.view, Qt::Key_Space);
    QCOMPARE(f.selected(), QStringList({"file00.txt", "file02.txt"}));
    QCOMPARE(f.currentName(), QString("file02.txt"));

    // Ctrl-Space toggles the row under the cursor and leaves the cursor there.
    QTest::keyClick(&f.view, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(f.selected(), QStringList({"file00.txt"}));
    QCOMPARE(f.currentName(), QString("file02.txt"));

    // On the last row the cursor stays.
    f.click("file09.txt");
    QTest::keyClick(&f.view, Qt::Key_Space);
    QCOMPARE(f.selected(), QStringList({"file09.txt"}));
    QCOMPARE(f.currentName(), QString("file09.txt"));
}

void TestFileView::typingOpensTheQuickSearchAndJumps()
{
    Fixture f;
    QVERIFY(f.listed);
    QCOMPARE(f.view.currentIndex().row(), 0);

    // Typing letters opens the small field and jumps to what answers.
    QTest::keyClicks(&f.view, "file0");
    QCOMPARE(f.view.currentIndex().row(), f.rowOf("file00.txt"));
    QVERIFY(f.view.findChild<QLabel *>());
    QVERIFY(f.view.findChild<QLabel *>()->isVisible());

    // Enter keeps what it found, and the keys act as themselves again.
    QTest::keyClick(&f.view, Qt::Key_Return);
    QVERIFY(!f.view.findChild<QLabel *>()->isVisible());
    QCOMPARE(f.view.currentIndex().row(), f.rowOf("file00.txt"));
    QTest::keyClick(&f.view, Qt::Key_Space);
    // Space marks, as it does when the quick search never was: the cursor
    // moves down one row, the mark stays where it was.
    QCOMPARE(f.view.selectionModel()->selectedRows().size(), 1);
    QVERIFY(f.view.selectionModel()->isSelected(f.view.model()->index(f.rowOf("file00.txt"), 0)));
}

void TestFileView::theQuickSearchTakesWildcardsAndEscapeGoesBack()
{
    Fixture f;
    QVERIFY(f.listed);
    const int folderRow = f.rowOf("folder-a");
    f.view.selectionModel()->setCurrentIndex(f.view.model()->index(folderRow, 0),
                                             QItemSelectionModel::NoUpdate);

    // Nothing is named like this: the field says so, and the cursor stays.
    QTest::keyClicks(&f.view, "zzz");
    QCOMPARE(f.view.currentIndex().row(), folderRow);
    QVERIFY(f.view.findChild<QLabel *>()->isVisible());
    QVERIFY(f.view.findChild<QLabel *>()->text().contains(
                QStringLiteral("nothing")));

    // Escape puts the cursor back and is over with it.
    QTest::keyClick(&f.view, Qt::Key_Escape);
    QVERIFY(!f.view.findChild<QLabel *>()->isVisible());
    QCOMPARE(f.view.currentIndex().row(), folderRow);

    // A pattern with a wildcard answers the way a filter would: file0? is
    // file00 and its brothers.
    QTest::keyClicks(&f.view, "file0?");
    QCOMPARE(f.view.currentIndex().row(), f.rowOf("file00.txt"));
    QTest::keyClick(&f.view, Qt::Key_Escape);
    QCOMPARE(f.view.currentIndex().row(), folderRow);
}

void TestFileView::arrowsSelectOneRowShiftExtends()
{
    Fixture f;
    QVERIFY(f.listed);
    f.click("file00.txt");

    // A plain arrow leaves one row selected.
    QTest::keyClick(&f.view, Qt::Key_Down);
    QCOMPARE(f.selected(), QStringList({"file01.txt"}));
    QCOMPARE(f.currentName(), QString("file01.txt"));

    QTest::keyClick(&f.view, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(&f.view, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(f.selected(), QStringList({"file01.txt", "file02.txt", "file03.txt"}));

    QTest::keyClick(&f.view, Qt::Key_Up);
    QCOMPARE(f.selected(), QStringList({"file02.txt"}));

    QTest::keyClick(&f.view, Qt::Key_Home);
    QCOMPARE(f.selected(), QStringList({"folder-a"}));
    QTest::keyClick(&f.view, Qt::Key_End);
    QCOMPARE(f.selected(), QStringList({"file09.txt"}));
}

void TestFileView::rightClickSelectsRowForMenu()
{
    Fixture f;
    QVERIFY(f.listed);
    QSignalSpy menu(&f.view, &FileView::contextMenuRequested);
    f.click("file02.txt");
    rightClick(f, f.pointOf("file05.txt"));
    QCOMPARE(f.selected(), QStringList({"file05.txt"}));
    QCOMPARE(menu.count(), 1);
    QCOMPARE(menu.at(0).at(0).toPoint(), f.pointOf("file05.txt"));

    // On a selected row, the selection stays as it is.
    f.click("file06.txt", Qt::ControlModifier);
    QCOMPARE(f.selected(), QStringList({"file05.txt", "file06.txt"}));
    rightClick(f, f.pointOf("file06.txt"));
    QCOMPARE(f.selected(), QStringList({"file05.txt", "file06.txt"}));
    QCOMPARE(menu.count(), 2);

    // From empty space too.
    rightClick(f, f.emptyPoint());
    QCOMPARE(f.selected(), QStringList({"file05.txt", "file06.txt"}));
    QCOMPARE(menu.count(), 3);

    // The keyboard's menu key asks for it at the cursor row.
    QTest::keyClick(&f.view, Qt::Key_Menu);
    QCOMPARE(menu.count(), 4);
    QVERIFY(menu.at(3).at(0).toPoint().y() > 0);
}

void TestFileView::dropLandsInFolderUnderPointer()
{
    Fixture f;
    QVERIFY(f.listed);
    QSignalSpy dropped(&f.view, &FileView::dropped);
    QMimeData mime;  // as if dragged in from another application
    mime.setUrls({QUrl::fromLocalFile(f.dir + "/elsewhere/photo.jpg")});

    // A folder row takes the drop.
    QVERIFY(dropAt(f, mime, f.pointOf("folder-b")));
    QCOMPARE(dropped.count(), 1);
    Drop result = dropFrom(dropped.at(0));
    QCOMPARE(fileNames(result.urls), QStringList({"photo.jpg"}));
    QCOMPARE(result.target, f.dir + "/folder-b");

    // A file row or empty space means the folder that is listed.
    QVERIFY(dropAt(f, mime, f.pointOf("file03.txt")));
    QCOMPARE(dropFrom(dropped.at(1)).target, f.dir);
    QVERIFY(dropAt(f, mime, f.emptyPoint()));
    QCOMPARE(dropFrom(dropped.at(2)).target, f.dir);
    QCOMPARE(dropped.count(), 3);
}

void TestFileView::dropChoosesCopyOrMove()
{
    Fixture f;
    QVERIFY(f.listed);
    QSignalSpy dropped(&f.view, &FileView::dropped);
    QMimeData external;
    external.setUrls({QUrl::fromLocalFile(f.dir + "/outside.txt")});
    QMimeData internal;  // dragged from the other pane
    internal.setUrls(external.urls());
    internal.setData(dragSourceFormat, (f.dir + "/elsewhere").toUtf8());

    // From another application: a copy, and never a question. Only a modifier
    // that says so makes it a move.
    for (Qt::KeyboardModifiers modifiers : {Qt::NoModifier, Qt::ControlModifier}) {
        QVERIFY(dropAt(f, external, f.emptyPoint(), modifiers));
        QCOMPARE(dropFrom(dropped.last()).action, Qt::CopyAction);
        QCOMPARE(dropFrom(dropped.last()).ask, false);
    }
    QVERIFY(dropAt(f, external, f.emptyPoint(), Qt::ShiftModifier));
    QCOMPARE(dropFrom(dropped.last()).action, Qt::MoveAction);
    QCOMPARE(dropFrom(dropped.last()).ask, false);
    QCOMPARE(dropped.count(), 3);

    // From this application, without a modifier: the pane has to ask.
    QVERIFY(dropAt(f, internal, f.emptyPoint()));
    Drop result = dropFrom(dropped.at(3));
    QCOMPARE(result.ask, true);
    QCOMPARE(result.action, Qt::CopyAction);
    QCOMPARE(result.target, f.dir);

    // With a modifier, that is the answer.
    QVERIFY(dropAt(f, internal, f.emptyPoint(), Qt::ShiftModifier));
    result = dropFrom(dropped.at(4));
    QCOMPARE(result.ask, false);
    QCOMPARE(result.action, Qt::MoveAction);

    QVERIFY(dropAt(f, internal, f.emptyPoint(), Qt::AltModifier));
    QCOMPARE(dropFrom(dropped.at(5)).action, Qt::MoveAction);

    QVERIFY(dropAt(f, internal, f.emptyPoint(), Qt::ControlModifier));
    result = dropFrom(dropped.at(6));
    QCOMPARE(result.ask, false);
    QCOMPARE(result.action, Qt::CopyAction);
}

void TestFileView::impossibleDropsAreRejected()
{
    Fixture f;
    QVERIFY(f.listed);
    QSignalSpy dropped(&f.view, &FileView::dropped);

    QMimeData sameFolder;
    sameFolder.setUrls({QUrl::fromLocalFile(f.dir + "/file00.txt")});
    sameFolder.setData(dragSourceFormat, f.dir.toUtf8());
    // Dragged out of this folder and dropped back onto it.
    QVERIFY(!dropAt(f, sameFolder, f.emptyPoint()));
    QVERIFY(!dropAt(f, sameFolder, f.pointOf("file04.txt")));

    // A folder onto itself.
    QMimeData ontoItself;
    ontoItself.setUrls({QUrl::fromLocalFile(f.dir + "/folder-a")});
    QVERIFY(!dropAt(f, ontoItself, f.pointOf("folder-a")));
    QVERIFY(!FileView::dropIsPossible(ontoItself, f.dir + "/folder-a/inner"));
    // … but next to it is fine.
    QVERIFY(FileView::dropIsPossible(ontoItself, f.dir + "/folder-b"));
    QVERIFY(dropAt(f, ontoItself, f.pointOf("folder-b")));

    // The listed folder itself is part of the dragged folder.
    QMimeData parent;
    parent.setUrls({QUrl::fromLocalFile(f.root.path())});
    QVERIFY(!FileView::dropIsPossible(parent, f.dir));

    // Nothing that can be dropped.
    QMimeData web;
    web.setUrls({QUrl("https://example.com/file.txt")});
    QVERIFY(!FileView::dropIsPossible(web, f.dir));
    QVERIFY(!dropAt(f, web, f.emptyPoint()));
    QMimeData text;
    text.setText("hello");
    QVERIFY(!FileView::dropIsPossible(text, f.dir));
    QVERIFY(!FileView::dropIsPossible(text, QString()));
    QCOMPARE(dropped.count(), 1);
}

void TestFileView::dragOverHighlightsFolderRow()
{
    Fixture f;
    QVERIFY(f.listed);
    QWidget *viewport = f.view.viewport();
    QMimeData external;
    external.setUrls({QUrl::fromLocalFile(f.dir + "/outside.txt")});

    QCOMPARE(f.view.dropRow(), -1);
    QDragEnterEvent enter(f.pointOf("folder-b"), kCopyOrMove, &external, Qt::NoButton,
                          Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(f.view.dropRow(), f.rowOf("folder-b"));

    // Over a file, the drop goes to the listed folder: no row is highlighted.
    QDragMoveEvent overFile(f.pointOf("file04.txt"), kCopyOrMove, &external, Qt::NoButton,
                            Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &overFile);
    QVERIFY(overFile.isAccepted());
    QCOMPARE(f.view.dropRow(), -1);

    // The drag leaves.
    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(viewport, &leave);
    QCOMPARE(f.view.dropRow(), -1);

    // What can't be dropped isn't accepted, and isn't highlighted either.
    QMimeData itself;
    itself.setUrls({QUrl::fromLocalFile(f.dir + "/folder-a")});
    QDragEnterEvent refused(f.pointOf("folder-a"), kCopyOrMove, &itself, Qt::NoButton,
                            Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &refused);
    QVERIFY(!refused.isAccepted());
    QCOMPARE(f.view.dropRow(), -1);
}

void TestFileView::narrowPanesDropColumns()
{
    Fixture f;
    QVERIFY(f.listed);
    const auto shown = [&]() {
        QStringList columns;
        for (int column = 0; column < FileModel::ColumnCount; ++column) {
            if (!f.view.isColumnHidden(column)) {
                columns.append(f.model.headerData(column, Qt::Horizontal).toString());
            }
        }
        return columns;
    };

    f.view.resize(900, 470);
    QCOMPARE(shown(), QStringList({"Name", "Size", "Type", "Modified"}));

    // Room for the names first: the type goes, then the date, then the size.
    f.view.resize(520, 470);
    QCOMPARE(shown(), QStringList({"Name", "Size", "Modified"}));
    f.view.resize(300, 470);
    QCOMPARE(shown(), QStringList({"Name", "Size"}));
    f.view.resize(190, 470);
    QCOMPARE(shown(), QStringList({"Name"}));
    QVERIFY(f.view.columnWidth(FileModel::NameColumn) >= 150);

    // And back again.
    f.view.resize(900, 470);
    QCOMPARE(shown(), QStringList({"Name", "Size", "Type", "Modified"}));
    QCOMPARE(f.proxy.columnCount(), FileModel::ColumnCount);
}

QTEST_MAIN(TestFileView)
#include "fileview.moc"
