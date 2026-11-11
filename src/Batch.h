// A run of labels from one template.
//
// The text of a label may hold fields in braces: "Rack {n:03}", "{name} — {date}".
// A run fills them once per label, from a counter, the clock, or the rows of a
// table (CSV or JSON) handed in by other software. All labels of a run travel in
// one job, because every job ends with a feed and a cut.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "Engine.h"

namespace ptouch {

using Row = QMap<QString, QString>;

struct DataSet {
    QStringList fields;   // column names, in the order of the source
    QList<Row> rows;
    QString error;        // set when the data could not be read
};

// Reads a table. JSON is recognised by its first character — an array of objects,
// or one object per line; everything else is CSV with a header row, separated by
// comma, semicolon or tab, whichever the header uses.
DataSet parseData(const QByteArray &data);
DataSet parseCsv(const QString &text);
DataSet parseJson(const QByteArray &data);

// Fills the fields of `text`.
//   {n} {n:03}             the counter, optionally padded to a width
//   {date} {date:%d.%m.%Y} today; strftime or Qt format, ISO when none is given
//   {time} {time:%H:%M:%S} now
//   {anything else}        the column of that name in `row`
//   {{ and }}              literal braces
// A column wins over a built-in of the same name. Fields nothing provides stay as
// they are written and are listed in `unknown`.
QString expandTemplate(const QString &text, const Row &row, qint64 n, const QDateTime &now,
                       QStringList *unknown = nullptr);

struct RunOptions {
    qint64 start = 1;     // first value of {n}
    qint64 step = 1;
    int count = 0;        // labels to make; 0 = one per row, or a single label
    QDateTime now = QDateTime::currentDateTime();
};

struct Batch {
    QList<Spec> specs;      // one per label, fields filled in
    QList<Layout> layouts;
    QStringList errors;     // unknown fields and the like; do not print then
    QStringList warnings;

    int size() const { return int(specs.size()); }
    double lengthPt() const { return layouts.isEmpty() ? 0 : layouts.first().lengthPt; }
};

// One label per row of `data` (or `count` labels when there is none), laid out
// with the settings of `pattern`. A job has one page size, so every label of a
// run gets the length of the longest one.
Batch buildBatch(const Spec &pattern, const DataSet &data, const RunOptions &options);

} // namespace ptouch
