#include "tools/LineDiff.h"

#include <QDebug>
#include <QHash>
#include <QStringView>

#include <algorithm>
#include <limits>

namespace LineDiff {

namespace {

// How far the search goes from each end of a region, in steps, before it
// stops looking for the shortest sequence of edits and cuts the region up
// instead. Every further step costs more than the one before it, and a region
// with more edits than this is no pleasure to read either way.
constexpr int kMaxLineSteps = 512;
constexpr int kMaxCharSteps = 16;
// The same limit in work done, for text where the lines repeat and every step
// runs through many of them.
constexpr qint64 kMaxLineWork = 1'000'000;
constexpr qint64 kMaxCharWork = 2'000;
// What all the searches of one comparison may spend. It is far more than any
// ordinary pair of files needs; past it, what is left is shown as changed.
constexpr qint64 kLineBudget = 300'000'000;
constexpr qint64 kCharBudget = 2'000'000;
// How many lines may be looked at for the ones that occur once on both sides,
// as a multiple of the length of the two texts.
constexpr qint64 kAnchorFactor = 8;

struct Point {
    int a = 0;
    int b = 0;
};

struct Range {
    int start = 0;
    int end = 0;

    int length() const
    {
        return end - start;
    }
};

enum class Tag {
    Equal,    // the same on both sides
    Remove,   // only on the left
    Insert,   // only on the right
    Replace,  // all of the left instead of all of the right
};

template <typename T>
int commonPrefix(const T *a, const T *b, int length)
{
    int common = 0;
    while (common < length && a[common] == b[common]) {
        ++common;
    }
    return common;
}

template <typename T>
int commonSuffix(const T *a, const T *b, int length)
{
    int common = 0;
    while (common < length && a[length - 1 - common] == b[length - 1 - common]) {
        ++common;
    }
    return common;
}

// A cut is only good when it is inside the region and leaves something to do
// on both sides of it.
bool isInside(const Point &point, int n, int m)
{
    return point.a >= 0 && point.b >= 0 && point.a <= n && point.b <= m
           && (point.a > 0 || point.b > 0) && (point.a < n || point.b < m);
}

// The workspace of Myers' search: how far each diagonal was reached, from the
// start and from the end. A diagonal is the set of places with the same
// (left position minus right position); `offset` keeps the index positive.
struct Search {
    int offset = 0;
    int maxSteps = 0;
    qint64 maxWork = 0;
    // Work that is left to all the searches of one comparison.
    qint64 *budget = nullptr;
    QVector<int> forward;
    QVector<int> backward;
    qint64 work = 0;

    Search(int steps, qint64 workLimit, qint64 *left)
        : offset(steps + 1)
        , maxSteps(steps)
        , maxWork(workLimit)
        , budget(left)
        , forward(2 * steps + 3, 0)
        , backward(2 * steps + 3, 0)
    {
    }
};

struct Found {
    bool met = false;    // the search from the start and the one from the end met
    Point point;         // a place where the shortest path can be cut in half
    Point furthest;      // how far the search from the start came
};

// Myers' algorithm, from both ends at once (the "middle snake" variant, which
// needs no more memory than the diagonals do): over a[0..n) and b[0..m), the
// place where the shortest path can be cut in half, unless the search runs out
// of steps or of work first.
template <typename T>
Found search(const T *a, int n, const T *b, int m, Search &s)
{
    Found found;
    s.work = 0;
    if (s.budget && *s.budget <= 0) {
        return found;
    }
    const auto spent = [&] {
        return s.work > s.maxWork || (s.budget && *s.budget - s.work <= 0);
    };
    const int delta = n - m;
    const bool odd = (delta & 1) != 0;
    const int offset = s.offset;
    int *const forward = s.forward.data();
    int *const backward = s.backward.data();
    forward[offset + 1] = 0;
    backward[offset + 1] = 0;
    int best = -1;
    for (int d = 0; d <= s.maxSteps; ++d) {
        s.work += 4 * d + 4;
        // From the start: the furthest point on every diagonal that d steps reach.
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && forward[offset + k - 1] < forward[offset + k + 1])) {
                x = forward[offset + k + 1];  // a step down: a line of b comes in
            } else {
                x = forward[offset + k - 1] + 1;  // a step right: a line of a goes
            }
            // The place right after the step, where the run of the same on this diagonal begins.
            const Point afterStep{x, x - k};
            int y = x - k;
            while (x < n && y >= 0 && y < m && a[x] == b[y]) {
                ++x;
                ++y;
                ++s.work;
            }
            forward[offset + k] = x;
            if (x + y > best) {
                best = x + y;
                found.furthest = Point{x, y};
            }
            // The search from the end has been one step behind. On the diagonals
            // it has reached, the two meet where the one gets as far as the other.
            if (odd && qAbs(k - delta) < d && x >= n - backward[offset + delta - k]) {
                if (isInside(afterStep, n, m)) {
                    found.point = afterStep;
                    found.met = true;
                    break;
                }
            }
            if (spent()) {
                break;
            }
        }
        if (found.met || spent()) {
            break;
        }
        // From the end, the same way with the ends swapped.
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && backward[offset + k - 1] < backward[offset + k + 1])) {
                x = backward[offset + k + 1];
            } else {
                x = backward[offset + k - 1] + 1;
            }
            int y = x - k;
            while (x < n && y >= 0 && y < m && a[n - 1 - x] == b[m - 1 - y]) {
                ++x;
                ++y;
                ++s.work;
            }
            backward[offset + k] = x;
            // Here the search from the start is exactly as far: the same diagonal, counted from the other end.
            if (!odd && qAbs(delta - k) <= d && forward[offset + delta - k] >= n - x) {
                const Point point{n - x, m - y};
                if (isInside(point, n, m)) {
                    found.point = point;
                    found.met = true;
                    break;
                }
            }
            if (spent()) {
                break;
            }
        }
        if (found.met || spent()) {
            break;
        }
    }
    if (s.budget) {
        *s.budget -= s.work;
    }
    return found;
}

// The longest run of pairs whose positions grow on both sides (patience
// sorting). The pairs come sorted by their left position.
QVector<Point> longestGrowingRun(QVector<Point> pairs)
{
    // For every length of run: the pair that ends it lowest so far, ...
    const QVector<Point> original = pairs;
    QVector<int> ends;
    // ... and which pair comes before each pair in such a run.
    QVector<int> previous(pairs.size(), -1);
    for (int i = 0; i < pairs.size(); ++i) {
        const int b = pairs.at(i).b;
        int low = 0;
        int high = int(ends.size());
        while (low < high) {
            const int middle = (low + high) / 2;
            if (pairs.at(ends.at(middle)).b < b) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low == ends.size()) {
            ends.append(i);
        } else {
            ends[low] = i;
        }
        if (low > 0) {
            previous[i] = ends.at(low - 1);
        }
    }
    // The run is the chain of pairs, from the last one backwards.
    pairs.clear();
    for (int i = ends.isEmpty() ? -1 : ends.last(); i >= 0; i = previous.at(i)) {
        pairs.append(original.at(i));
    }
    std::reverse(pairs.begin(), pairs.end());
    return pairs;
}

// A line as it compares with another: what the options say is the same.
QString comparableLine(const QString &line, const Options &options)
{
    if (!options.ignoreTrailingWhitespace) {
        return line;
    }
    qsizetype end = line.size();
    while (end > 0 && line.at(end - 1).isSpace()) {
        --end;
    }
    return line.left(end);
}

// Whether a line is blank: nothing in it, or only whitespace.
bool isBlankLine(const QString &line)
{
    for (const QChar c : line) {
        if (!c.isSpace()) {
            return false;
        }
    }
    return true;
}

// A stretch of both texts to compare: [aStart, aEnd) of the left and
// [bStart, bEnd) of the right.
struct Region {
    int aStart = 0;
    int aEnd = 0;
    int bStart = 0;
    int bEnd = 0;
};

// One outcome of the walk: a run of lines that are handled one way.
struct Part {
    Tag tag = Tag::Equal;
    Region region;   // for a region that is split further
    int aStart = 0;  // for a settled stretch
    int aEnd = 0;
    int bStart = 0;
    int bEnd = 0;
};

// The regions and settled stretches, in the order they appear. Processed from
// the front, so that a region splits into the pieces that take its place.
QList<Part> plan(const QVector<int> &idsA, const QVector<int> &idsB,
                 const QHash<int, int> &countA, const QHash<int, int> &countB,
                 qint64 *budget)
{
    // The stretches that are settled, in the order they appear, and the
    // regions that still have to be looked at, in front of what follows them.
    QList<Part> done;
    QList<Part> work;
    work.append(Part{Tag::Replace, {}, 0, int(idsA.size()), 0, int(idsB.size())});

    while (!work.isEmpty()) {
        const Part part = work.takeFirst();
        if (part.tag != Tag::Replace) {
            // A settled stretch: it goes out as it is.
            done.append(part);
            continue;
        }
        // A region's extent is in the part's own bounds.
        const Region region{part.aStart, part.aEnd, part.bStart, part.bEnd};
        int aStart = region.aStart;
        int aEnd = region.aEnd;
        int bStart = region.bStart;
        int bEnd = region.bEnd;

        // What is the same at the start goes out first.
        int same = 0;
        while (aStart + same < aEnd && bStart + same < bEnd
               && idsA.at(aStart + same) == idsB.at(bStart + same)) {
            ++same;
        }
        if (same > 0) {
            done.append(Part{Tag::Equal, {}, aStart, aStart + same, bStart, bStart + same});
            aStart += same;
            bStart += same;
        }
        // ... and at the end.
        same = 0;
        while (aEnd - same > aStart && bEnd - same > bStart
               && idsA.at(aEnd - 1 - same) == idsB.at(bEnd - 1 - same)) {
            ++same;
        }
        if (same > 0) {
            // The suffix is settled, but it comes after what is looked at
            // next, so it waits in the work in front of what follows it.
            work.prepend(Part{Tag::Equal, {}, aEnd - same, aEnd, bEnd - same, bEnd});
            aEnd -= same;
            bEnd -= same;
        }
        if (aStart == aEnd || bStart == bEnd) {
            work.prepend(Part{aStart == aEnd ? Tag::Insert : Tag::Remove, {}, aStart, aEnd, bStart, bEnd});
            continue;
        }

        // The lines that occur once on each side anchor the comparison: what
        // is between them is looked at next, one stretch per gap.
        QList<Point> pairs;
        for (int a = aStart; a < aEnd; ++a) {
            if (countA.value(idsA.at(a)) == 1 && countB.value(idsA.at(a)) == 1) {
                pairs.append(Point{a, -1});
            }
        }
        if (!pairs.isEmpty()) {
            // Their places on the right, and only those that grow in step.
            QList<QPair<int, int>> rightOf;
            QHash<int, int> rightPlace;
            for (int b = bStart; b < bEnd; ++b) {
                rightPlace.insert(idsB.at(b), b);
            }
            int kept = 0;
            for (const Point &pair : pairs) {
                const int place = rightPlace.value(idsA.at(pair.a), -1);
                if (place >= 0) {
                    pairs[kept] = Point{pair.a, place};
                    ++kept;
                }
            }
            pairs.resize(kept);
            const QVector<Point> anchors = longestGrowingRun(pairs);
            if (!anchors.isEmpty()) {
                QList<Part> pieces;
                int cursorA = aStart;
                int cursorB = bStart;
                for (const Point &anchor : anchors) {
                    if (anchor.a > cursorA || anchor.b > cursorB) {
                        pieces.append(Part{Tag::Replace, {}, cursorA, anchor.a, cursorB, anchor.b});
                    }
                    pieces.append(Part{Tag::Equal, {}, anchor.a, anchor.a + 1, anchor.b, anchor.b + 1});
                    cursorA = anchor.a + 1;
                    cursorB = anchor.b + 1;
                }
                if (cursorA < aEnd || cursorB < bEnd) {
                    pieces.append(Part{Tag::Replace, {}, cursorA, aEnd, cursorB, bEnd});
                }
                if (pieces.size() > 1 || pieces.first().tag != Tag::Replace) {
                    // The pieces take the region's place; they come before
                    // whatever is in the work already.
                    for (int i = pieces.size() - 1; i >= 0; --i) {
                        work.prepend(pieces.at(i));
                    }
                    continue;
                }
            }
        }

        // No anchors, or none that help: Myers' search, up to what it is worth.
        // It cuts the region in half where the shortest path through the edits
        // passes; each half is looked at next.
        const int n = aEnd - aStart;
        const int m = bEnd - bStart;
        Search lineSearch(kMaxLineSteps, kMaxLineWork, budget);
        const Found found = search(idsA.constData() + aStart, n, idsB.constData() + bStart, m,
                                   lineSearch);
        if (found.met) {
            // The cut is relative to the region; in absolute places it is
            // aStart + cut.a on the left and bStart + cut.b on the right.
            const Point cut{aStart + found.point.a, bStart + found.point.b};
            work.prepend(Part{Tag::Replace, {}, cut.a, aEnd, cut.b, bEnd});
            work.prepend(Part{Tag::Replace, {}, aStart, cut.a, bStart, cut.b});
            continue;
        }
        // Nothing more is worth finding here: the stretch is shown as it is.
        done.append(part);
    }
    return done;
}

}  // namespace

Result compare(const Text &left, const Text &right, const Options &options)
{
    Result result;

    // Every line gets a number, the same number for the same line. Then whole
    // files can be told apart with numbers alone, however long they are.
    QHash<QString, int> dictionary;
    const auto identifier = [&dictionary](const QString &line) {
        const auto found = dictionary.constFind(line);
        if (found != dictionary.constEnd()) {
            return found.value();
        }
        const int id = int(dictionary.size());
        dictionary.insert(line, id);
        return id;
    };

    QVector<int> idsA(left.lines.size());
    QVector<int> idsB(right.lines.size());
    QHash<int, int> countA;
    QHash<int, int> countB;
    for (int i = 0; i < left.lines.size(); ++i) {
        const QString line = comparableLine(left.lines.at(i), options);
        idsA[i] = identifier(line);
        ++countA[idsA[i]];
    }
    for (int i = 0; i < right.lines.size(); ++i) {
        const QString line = comparableLine(right.lines.at(i), options);
        idsB[i] = identifier(line);
        ++countB[idsB[i]];
    }

    qint64 budget = kLineBudget;
    QList<Part> parts = plan(idsA, idsB, countA, countB, &budget);

    // A run of stretches that are only on one side lines up into one
    // comparison: the lines of both sides pair up as far as there are, and
    // what is over on either side is removed or added. Whatever the order of
    // the removals and additions in the run.
    {
        QList<Part> lined;
        int leftFrom = -1;
        int leftTo = -1;
        int rightFrom = -1;
        int rightTo = -1;
        for (const Part &part : parts) {
            const bool onOneSide = part.tag == Tag::Remove || part.tag == Tag::Insert;
            if (onOneSide) {
                if (leftFrom < 0) {
                    leftFrom = part.aStart;
                    leftTo = part.aEnd;
                    rightFrom = part.bStart;
                    rightTo = part.bEnd;
                } else {
                    leftTo = part.aEnd;
                    rightTo = part.bEnd;
                }
                continue;
            }
            if (leftFrom >= 0) {
                lined.append(Part{Tag::Replace, {}, leftFrom, leftTo, rightFrom, rightTo});
                leftFrom = -1;
            }
            lined.append(part);
        }
        if (leftFrom >= 0) {
            lined.append(Part{Tag::Replace, {}, leftFrom, leftTo, rightFrom, rightTo});
        }
        parts = lined;
    }
    qDebug() << "[probe compare] idsA" << idsA << "idsB" << idsB << "parts" << parts.size();
    for (const Part &part : parts) {
        qDebug() << "  part tag" << int(part.tag) << part.aStart << part.aEnd << part.bStart << part.bEnd;
    }

    for (const Part &part : parts) {
        const int leftCount = part.aEnd - part.aStart;
        const int rightCount = part.bEnd - part.bStart;
        switch (part.tag) {
        case Tag::Equal:
            for (int i = 0; i < leftCount; ++i) {
                Row row;
                row.kind = Kind::Equal;
                row.leftNumber = part.aStart + i + 1;
                row.rightNumber = part.bStart + i + 1;
                row.left = left.lines.at(part.aStart + i);
                row.right = right.lines.at(part.bStart + i);
                ++result.same;
                result.rows.append(row);
            }
            break;
        case Tag::Remove: {
            for (int i = 0; i < leftCount; ++i) {
                Row row;
                row.leftNumber = part.aStart + i + 1;
                row.left = left.lines.at(part.aStart + i);
                if (options.ignoreBlankLines && isBlankLine(row.left)) {
                    row.kind = Kind::Ignored;
                } else {
                    row.kind = Kind::Removed;
                    ++result.removed;
                }
                result.rows.append(row);
            }
            break;
        }
        case Tag::Insert: {
            for (int i = 0; i < rightCount; ++i) {
                Row row;
                row.rightNumber = part.bStart + i + 1;
                row.right = right.lines.at(part.bStart + i);
                if (options.ignoreBlankLines && isBlankLine(row.right)) {
                    row.kind = Kind::Ignored;
                } else {
                    row.kind = Kind::Added;
                    ++result.added;
                }
                result.rows.append(row);
            }
            break;
        }
        case Tag::Replace: {
            // The lines line up as far as there are on both sides; whatever is
            // left over is only on one side.
            const int paired = qMin(leftCount, rightCount);
            for (int i = 0; i < paired; ++i) {
                Row row;
                row.leftNumber = part.aStart + i + 1;
                row.rightNumber = part.bStart + i + 1;
                row.left = left.lines.at(part.aStart + i);
                row.right = right.lines.at(part.bStart + i);
                if (comparableLine(row.left, options) == comparableLine(row.right, options)) {
                    // What the options call the same is the same.
                    row.kind = Kind::Equal;
                    ++result.same;
                } else {
                    row.kind = Kind::Changed;
                    const LineChanges changes = withinLine(row.left, row.right);
                    row.leftSpans = changes.left;
                    row.rightSpans = changes.right;
                    ++result.removed;
                    ++result.added;
                }
                result.rows.append(row);
            }
            for (int i = paired; i < leftCount; ++i) {
                Row row;
                row.leftNumber = part.aStart + i + 1;
                row.left = left.lines.at(part.aStart + i);
                if (options.ignoreBlankLines && isBlankLine(row.left)) {
                    row.kind = Kind::Ignored;
                } else {
                    row.kind = Kind::Removed;
                    ++result.removed;
                }
                result.rows.append(row);
            }
            for (int i = paired; i < rightCount; ++i) {
                Row row;
                row.rightNumber = part.bStart + i + 1;
                row.right = right.lines.at(part.bStart + i);
                if (options.ignoreBlankLines && isBlankLine(row.right)) {
                    row.kind = Kind::Ignored;
                } else {
                    row.kind = Kind::Added;
                    ++result.added;
                }
                result.rows.append(row);
            }
            break;
        }
        }
    }
    return result;
}

Text split(const QString &text, const Options &options)
{
    Text result;
    QString current;
    bool afterCarriageReturn = false;
    for (const QChar c : text) {
        if (afterCarriageReturn && c == QLatin1Char('\n')) {
            // The line break was already taken by the "\r" in front of it.
            afterCarriageReturn = false;
            continue;
        }
        afterCarriageReturn = false;
        if (c == QLatin1Char('\n') || (options.ignoreLineEndings && c == QLatin1Char('\r'))) {
            result.lines.append(current);
            current.clear();
            result.endsWithLineBreak = true;
            afterCarriageReturn = options.ignoreLineEndings && c == QLatin1Char('\r');
            continue;
        }
        current.append(c);
    }
    if (!current.isEmpty()) {
        result.lines.append(current);
        result.endsWithLineBreak = false;
    } else if (result.lines.isEmpty() && !text.isEmpty()) {
        // Only breaks: the lines are all empty, and a break ends the last one.
        result.lines.append(QString());
    }
    return result;
}

LineChanges withinLine(const QString &left, const QString &right)
{
    LineChanges changes;
    // What the two lines have in common at the start and at the end, counted
    // in UTF-16 units like the spans.
    const int length = qMin(left.size(), right.size());
    int start = 0;
    while (start < length && left.at(start) == right.at(start)) {
        ++start;
    }
    if (start == length) {
        // One line is completely in the other: what is over differs, on the
        // side that has it.
        return changes;
    }
    int end = 0;
    while (end < length - start && left.at(left.size() - 1 - end) == right.at(right.size() - 1 - end)) {
        ++end;
    }
    const int leftStart = start;
    const int leftEnd = left.size() - end;
    const int rightStart = start;
    const int rightEnd = right.size() - end;
    if (leftEnd > leftStart) {
        changes.left.append({leftStart, leftEnd - leftStart});
    }
    if (rightEnd > rightStart) {
        changes.right.append({rightStart, rightEnd - rightStart});
    }
    return changes;
}

}  // namespace LineDiff
