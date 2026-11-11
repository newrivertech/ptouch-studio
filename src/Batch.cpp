#include "Batch.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <ctime>

namespace ptouch {
namespace {

QString jsonToString(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::String:
        return v.toString();
    case QJsonValue::Bool:
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: {
        const double d = v.toDouble();
        // 42 should not turn into 42.0 on a label
        if (d == double(qint64(d)))
            return QString::number(qint64(d));
        return QString::number(d, 'g', 15);
    }
    case QJsonValue::Array:
    case QJsonValue::Object:
        return QString::fromUtf8(QJsonDocument::fromVariant(v.toVariant())
                                     .toJson(QJsonDocument::Compact));
    default:
        return {};
    }
}

void addObject(DataSet &set, const QJsonObject &o)
{
    Row row;
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!set.fields.contains(it.key()))
            set.fields << it.key();
        row.insert(it.key(), jsonToString(it.value()));
    }
    set.rows << row;
}

// The separator is whichever of , ; and tab the header uses most, quotes aside.
QChar detectSeparator(const QString &text)
{
    int counts[3] = {0, 0, 0};
    const QChar candidates[3] = {QLatin1Char(','), QLatin1Char(';'), QLatin1Char('\t')};
    bool quoted = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"'))
            quoted = !quoted;
        else if (!quoted && (c == QLatin1Char('\n') || c == QLatin1Char('\r')))
            break;
        else if (!quoted)
            for (int i = 0; i < 3; ++i)
                if (c == candidates[i])
                    ++counts[i];
    }
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (counts[i] > counts[best])
            best = i;
    return candidates[best];
}

// RFC 4180: quoted cells may hold separators, line breaks and doubled quotes.
QList<QStringList> splitCsv(const QString &text, QChar separator)
{
    QList<QStringList> records;
    QStringList record;
    QString cell;
    bool quoted = false, cellStarted = false;
    const auto endCell = [&] {
        record << cell;
        cell.clear();
        cellStarted = false;
    };
    const auto endRecord = [&] {
        endCell();
        records << record;
        record.clear();
    };
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    cell += c;
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell += c;
            }
        } else if (c == QLatin1Char('"') && !cellStarted) {
            quoted = true;
            cellStarted = true;
        } else if (c == separator) {
            endCell();
        } else if (c == QLatin1Char('\n')) {
            endRecord();
        } else if (c != QLatin1Char('\r')) {
            cell += c;
            cellStarted = true;
        }
    }
    if (cellStarted || !cell.isEmpty() || !record.isEmpty())
        endRecord();
    return records;
}

QString formatDateTime(const QDateTime &now, const QString &format, const QString &fallback)
{
    if (format.isEmpty())
        return now.toString(fallback);
    if (!format.contains(QLatin1Char('%')))
        return now.toString(format);
    const std::time_t t = std::time_t(now.toSecsSinceEpoch());
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buffer[256];
    const size_t n = std::strftime(buffer, sizeof(buffer), format.toLocal8Bit().constData(), &tm);
    return QString::fromLocal8Bit(buffer, int(n));
}

QString formatCounter(qint64 n, const QString &format)
{
    bool ok = false;
    const int width = format.toInt(&ok);
    if (!ok || width <= 0)
        return QString::number(n);
    const QChar pad = format.startsWith(QLatin1Char('0')) ? QLatin1Char('0') : QLatin1Char(' ');
    if (n < 0 && pad == QLatin1Char('0'))
        return QLatin1Char('-') + QString::number(-n).rightJustified(width - 1, pad);
    return QString::number(n).rightJustified(width, pad);
}

// Columns are matched by name, forgiving case and surrounding blanks.
bool lookup(const Row &row, const QString &name, QString *value)
{
    auto it = row.constFind(name);
    if (it == row.constEnd()) {
        for (it = row.constBegin(); it != row.constEnd(); ++it)
            if (it.key().compare(name, Qt::CaseInsensitive) == 0)
                break;
    }
    if (it == row.constEnd())
        return false;
    *value = it.value();
    return true;
}

} // namespace

DataSet parseCsv(const QString &input)
{
    DataSet set;
    QString text = input;
    if (text.startsWith(QChar(0xFEFF)))   // spreadsheets like to write a byte order mark
        text.remove(0, 1);

    const QList<QStringList> records = splitCsv(text, detectSeparator(text));
    if (records.isEmpty()) {
        set.error = QStringLiteral("the data is empty");
        return set;
    }
    for (const QString &name : records.first())
        set.fields << name.trimmed();
    for (int r = 1; r < records.size(); ++r) {
        const QStringList &cells = records.at(r);
        const bool blank = std::all_of(cells.begin(), cells.end(),
                                       [](const QString &c) { return c.trimmed().isEmpty(); });
        if (blank)
            continue;
        Row row;
        for (int c = 0; c < set.fields.size(); ++c)
            if (!set.fields.at(c).isEmpty())
                row.insert(set.fields.at(c), cells.value(c).trimmed());
        set.rows << row;
    }
    set.fields.removeAll(QString());
    if (set.rows.isEmpty())
        set.error = QStringLiteral("the data has a header row but no rows below it");
    return set;
}

DataSet parseJson(const QByteArray &data)
{
    DataSet set;
    QJsonParseError whole{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &whole);
    if (whole.error == QJsonParseError::NoError) {
        if (doc.isObject()) {
            addObject(set, doc.object());
        } else {
            for (const QJsonValue &v : doc.array()) {
                if (!v.isObject()) {
                    set.error = QStringLiteral("the JSON array must hold objects, "
                                               "one per label");
                    return set;
                }
                addObject(set, v.toObject());
            }
        }
    } else {
        // One object per line, as jq -c and most log-like exports write it.
        int number = 0;
        for (const QByteArray &line : data.split('\n')) {
            ++number;
            if (line.trimmed().isEmpty())
                continue;
            QJsonParseError e{};
            const QJsonDocument one = QJsonDocument::fromJson(line, &e);
            if (e.error != QJsonParseError::NoError || !one.isObject()) {
                set.error = QStringLiteral("line %1 is not a JSON object (%2)")
                                .arg(number).arg(e.errorString());
                return set;
            }
            addObject(set, one.object());
        }
    }
    if (set.rows.isEmpty() && set.error.isEmpty())
        set.error = QStringLiteral("the data holds no rows");
    return set;
}

DataSet parseData(const QByteArray &data)
{
    const QByteArray head = data.trimmed().left(1);
    if (head == "{" || head == "[")
        return parseJson(data);
    return parseCsv(QString::fromUtf8(data));
}

QString expandTemplate(const QString &text, const Row &row, qint64 n, const QDateTime &now,
                       QStringList *unknown)
{
    QString result;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        const bool doubled = i + 1 < text.size() && text.at(i + 1) == c;
        if ((c == QLatin1Char('{') || c == QLatin1Char('}')) && doubled) {
            result += c;
            ++i;
            continue;
        }
        if (c != QLatin1Char('{')) {
            result += c;
            continue;
        }
        const int close = text.indexOf(QLatin1Char('}'), i + 1);
        const QString inner = close < 0 ? QString() : text.mid(i + 1, close - i - 1);
        if (close < 0 || inner.contains(QLatin1Char('{')) || inner.contains(QLatin1Char('\n'))) {
            result += c;        // a lone brace is just a brace
            continue;
        }

        QString value;
        const int colon = inner.indexOf(QLatin1Char(':'));
        const QString name = (colon < 0 ? inner : inner.left(colon)).trimmed();
        const QString format = colon < 0 ? QString() : inner.mid(colon + 1);
        if (lookup(row, inner.trimmed(), &value) || lookup(row, name, &value)) {
            result += value;
        } else if (name == QLatin1String("n")) {
            result += formatCounter(n, format);
        } else if (name == QLatin1String("date")) {
            result += formatDateTime(now, format, QStringLiteral("yyyy-MM-dd"));
        } else if (name == QLatin1String("time")) {
            result += formatDateTime(now, format, QStringLiteral("HH:mm"));
        } else {
            if (unknown && !name.isEmpty() && !unknown->contains(name))
                *unknown << name;
            result += text.mid(i, close - i + 1);
        }
        i = close;
    }
    return result;
}

Batch buildBatch(const Spec &pattern, const DataSet &data, const RunOptions &options)
{
    Batch batch;
    const bool hasData = !data.rows.isEmpty();
    int labels = hasData ? int(data.rows.size()) : 1;
    if (options.count > 0)
        labels = hasData ? std::min(labels, options.count) : options.count;

    QStringList unknown;
    for (int i = 0; i < labels; ++i) {
        const Row row = hasData ? data.rows.at(i) : Row();
        const qint64 n = options.start + qint64(i) * options.step;
        Spec spec = pattern;
        spec.text = expandTemplate(pattern.text, row, n, options.now, &unknown);
        spec.codeData = expandTemplate(pattern.codeData, row, n, options.now, &unknown);
        batch.specs << spec;
    }
    // Without a table a field nobody fills is taken as text that happens to have
    // braces in it. With one it is a misspelt column, and that is worth stopping for.
    if (hasData && !unknown.isEmpty())
        batch.errors << QStringLiteral("no such column: %1 (the data has: %2)")
                            .arg(unknown.join(QStringLiteral(", ")),
                                 data.fields.join(QStringLiteral(", ")));

    // Copies of a run are laid one after another, label by label: with several
    // pages in the job the order is otherwise up to CUPS.
    if (batch.specs.size() > 1 && pattern.copies > 1) {
        QList<Spec> repeated;
        for (Spec spec : std::as_const(batch.specs)) {
            spec.copies = 1;
            for (int c = 0; c < pattern.copies; ++c)
                repeated << spec;
        }
        batch.specs = repeated;
    }

    double longest = 0;
    for (const Spec &spec : std::as_const(batch.specs)) {
        batch.layouts << computeLayout(spec);
        longest = std::max(longest, batch.layouts.last().lengthPt);
    }
    if (batch.specs.size() > 1) {
        for (int i = 0; i < batch.specs.size(); ++i) {
            if (batch.layouts.at(i).lengthPt >= longest - 0.01)
                continue;
            batch.specs[i].autoLength = false;
            batch.specs[i].lengthMm = longest / PtPerMm;
            batch.layouts[i] = computeLayout(batch.specs.at(i));
        }
    }
    // A note that holds for every label is said once; others name their label.
    QStringList notes;
    QMap<QString, QList<int>> where;
    for (int i = 0; i < batch.layouts.size(); ++i)
        for (const QString &w : batch.layouts.at(i).warnings) {
            if (!notes.contains(w))
                notes << w;
            where[w] << i + 1;
        }
    for (const QString &w : std::as_const(notes)) {
        const QList<int> &labels = where[w];
        if (labels.size() == batch.size()) {
            batch.warnings << w;
        } else {
            QStringList numbers;
            for (int label : labels)
                numbers << QString::number(label);
            batch.warnings << QStringLiteral("label %1: %2")
                                  .arg(numbers.join(QStringLiteral(", ")), w);
        }
    }
    return batch;
}

} // namespace ptouch
