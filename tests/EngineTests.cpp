// Checks the measurements that were verified against the real device.
//
// The crux is font scaling: Qt converts point sizes against the device DPI. Scaling
// the coordinate system on top of that (600 dpi PDF) makes the text come out DPI/72
// too large while every other measurement looks right. So this does not merely do
// arithmetic — it rasterises the produced PDF and measures the ink.
#include "Batch.h"
#include "Config.h"
#include "Engine.h"
#include "Snmp.h"
#include "Status.h"

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace ptouch;

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    std::printf("%s  %s\n", condition ? "ok  " : "FAIL", qPrintable(what));
    if (!condition)
        ++failures;
}

void checkNear(double actual, double expected, double tolerance, const QString &what)
{
    const bool ok = std::fabs(actual - expected) <= tolerance;
    std::printf("%s  %s (%.2f, expected %.2f ± %.2f)\n",
                ok ? "ok  " : "FAIL", qPrintable(what), actual, expected, tolerance);
    if (!ok)
        ++failures;
}

// Rasterises the PDF at 10 pixels per millimetre and measures the actual ink.
struct Ink {
    bool valid = false;
    double pageWidthMm = 0, pageHeightMm = 0;
    double acrossMm = 0, alongMm = 0;
    double marginTopMm = 0, marginBottomMm = 0;
    double marginLeadMm = 0, marginTrailMm = 0;
};

Ink measurePdf(const QString &pdfPath, const QString &workDir)
{
    Ink ink;
    const QString stem = workDir + QStringLiteral("/raster");
    QProcess pdftoppm;
    pdftoppm.start(QStringLiteral("pdftoppm"),
                   {QStringLiteral("-r"), QStringLiteral("254"), QStringLiteral("-gray"),
                    QStringLiteral("-png"), pdfPath, stem});
    if (!pdftoppm.waitForFinished(30000) || pdftoppm.exitCode() != 0)
        return ink;   // without poppler-utils this part is skipped

    QImage image(stem + QStringLiteral("-1.png"));
    if (image.isNull())
        image = QImage(stem + QStringLiteral("-01.png"));
    if (image.isNull())
        return ink;

    image = image.convertToFormat(QImage::Format_Grayscale8);
    const int w = image.width(), h = image.height();
    int minX = w, maxX = -1, minY = h, maxY = -1;
    for (int y = 0; y < h; ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (line[x] < 128) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return ink;

    ink.valid = true;
    ink.pageWidthMm = w / 10.0;
    ink.pageHeightMm = h / 10.0;
    ink.acrossMm = (maxX - minX + 1) / 10.0;
    ink.alongMm = (maxY - minY + 1) / 10.0;
    ink.marginTopMm = minX / 10.0;
    ink.marginBottomMm = (w - 1 - maxX) / 10.0;
    ink.marginLeadMm = minY / 10.0;
    ink.marginTrailMm = (h - 1 - maxY) / 10.0;
    return ink;
}

void testTapeTable()
{
    check(tapes().size() == 8, "eight tape widths known");
    check(tapeFor(12.0) != nullptr, "12 mm is a known tape width");
    check(tapeFor(13.0) == nullptr, "13 mm is not a known tape width");
    check(tapeIndex(24.0) == 6, "24 mm sits between 21 and 36 mm");

    Spec s;
    s.tapeMm = 12.0;
    checkNear(s.tapePt(), 34.0, 0.01, "12 mm tape is 34 pt wide");
    checkNear(s.printablePt() / PtPerMm, 10.73, 0.05, "12 mm tape: printable height");
    s.tapeMm = 24.0;
    checkNear(s.printablePt() / PtPerMm, 18.06, 0.05, "24 mm tape: printable height");

    // The head is the limit whenever the tape could take more: 36 mm tape would
    // allow 192 dots, but a 128 dot head reaches only 18.06 mm of it.
    s.tapeMm = 36.0;
    s.model = QStringLiteral("PT-P710BT");
    checkNear(s.printablePt() / PtPerMm, 18.06, 0.05, "128 dot head limits wide tape");
    s.model = QStringLiteral("PT-P900Wc");
    checkNear(s.printablePt() / PtPerMm, 32.0, 0.6, "360 dpi head prints wider");
    s.tapeMm = 12.0;
    s.model = QStringLiteral("PT-2300");           // 112 dot head
    checkNear(s.printablePt() / PtPerMm, 10.73, 0.05, "narrow head, narrow tape unaffected");
}

void testAutoSizeFillsTape()
{
    // Automatic sizing has to use the printable height rather than the font
    // metrics, otherwise the text stays noticeably smaller than it could be.
    for (const double width : {12.0, 24.0}) {
        Spec s;
        s.tapeMm = width;
        s.text = QStringLiteral("Workshop");
        const Layout l = computeLayout(s);
        checkNear(l.inkHeight, s.printablePt(), 0.6,
                  QStringLiteral("%1 mm: text fills the printable height").arg(width));
        check(l.warnings.isEmpty(), QStringLiteral("%1 mm: no warning").arg(width));
    }
}

void testMultiLineShrinks()
{
    Spec one, two;
    one.text = QStringLiteral("Fuse box");
    two.text = QStringLiteral("Fuse box\nF3 16A");
    const Layout l1 = computeLayout(one);
    const Layout l2 = computeLayout(two);
    check(l2.sizePt < l1.sizePt, "two lines yield a smaller font");
    checkNear(l2.inkHeight, two.printablePt(), 0.6, "two lines fill the tape height");
}

void testFixedLength()
{
    Spec s;
    s.text = QStringLiteral("Left");
    s.autoLength = false;
    s.lengthMm = 60;
    const Layout l = computeLayout(s);
    checkNear(l.lengthPt / PtPerMm, 60.0, 0.01, "fixed length is honoured");
    check(!l.overflow, "short text fits the fixed length");
}

void testOverflowDetected()
{
    Spec s;
    s.text = QStringLiteral("A very long text that will not fit");
    s.autoLength = false;
    s.autoSize = false;
    s.sizePt = 30;
    s.lengthMm = 20;
    const Layout l = computeLayout(s);
    check(l.overflow, "overlong text is detected");
    check(!l.warnings.isEmpty(), "overlong text produces a warning");
}

void testPdfGeometry(const QString &workDir)
{
    Spec s;
    s.tapeMm = 12.0;
    s.text = QStringLiteral("Workshop");
    const Layout l = computeLayout(s);

    const QString pdf = workDir + QStringLiteral("/label.pdf");
    const PageSize page = writePdf(pdf, s, l);
    check(page.widthPt == 34, "PDF page is 34 pt wide");
    checkNear(page.heightPt / PtPerMm, l.lengthPt / PtPerMm, 0.5,
              "PDF page length matches the layout");

    const Ink ink = measurePdf(pdf, workDir);
    if (!ink.valid) {
        std::printf("skipped: pdftoppm not available\n");
        return;
    }
    checkNear(ink.pageWidthMm, 12.0, 0.15, "rasterised page is 12 mm wide");
    checkNear(ink.acrossMm, 10.6, 0.8, "ink uses the printable tape height");
    checkNear(ink.marginTopMm, ink.marginBottomMm, 0.5, "text is centred on the tape");
    // The measured margin is the configured one plus the glyph's side bearing,
    // which depends on the first and last letter — hence the generous tolerance.
    checkNear(ink.marginLeadMm, s.marginMm, 1.0, "leading side margin as configured");
    checkNear(ink.marginTrailMm, s.marginMm, 1.0, "trailing side margin as configured");
}

void testGlyphDetection()
{
    // Plain text and outline symbols draw as vectors; anything only the colour
    // bitmap font provides has to take the image path or the PDF loses it.
    Spec plain;
    plain.text = QStringLiteral("Workshop 12");
    check(!needsRasterGlyphs(plain), "plain text draws as outlines");

    Spec star;
    star.text = QStringLiteral("★ Shelf");
    check(!needsRasterGlyphs(star), "outline symbols draw as outlines");

    Spec emoji;
    emoji.text = QStringLiteral("🔧 Tool");
    check(needsRasterGlyphs(emoji), "pictographs need the image path");

    // The image has to carry actual ink, otherwise the emoji silently vanishes.
    const Layout l = computeLayout(emoji);
    const QImage image = renderToImage(emoji, l, 300);
    int dark = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 200)
                ++dark;
    check(dark > 500, "rendered image contains the pictograph");
    check(image.format() == QImage::Format_Grayscale8, "image is greyscale for the tape");
}

void testArtwork()
{
    // A barcode has to be sized by module width, not by available space: below
    // roughly a quarter millimetre per bar no scanner reads it.
    Spec bc;
    bc.tapeMm = 12.0;
    bc.text = QStringLiteral("Stock");
    bc.codeType = CodeType::Code128;
    bc.codeData = QStringLiteral("ABC-12345");
    const Layout lbc = computeLayout(bc);
    check(!lbc.artwork.isNull(), "barcode is rendered");
    const double moduleMm = lbc.artworkWidth / std::max(1, lbc.artwork.width()) / PtPerMm;
    checkNear(moduleMm, 0.28, 0.05, "barcode modules are wide enough to scan");
    check(lbc.lengthPt > bc.marginMm * 2 * PtPerMm + lbc.artworkWidth,
          "label grew to hold barcode and text");

    // Code 128 set B is ASCII only; anything else has to be refused rather than
    // silently mangled.
    QString error;
    check(renderCode(CodeType::Code128, QStringLiteral("Grüße"), &error).isNull()
              && !error.isEmpty(), "non-ASCII is rejected for Code 128");

    if (qrAvailable()) {
        Spec qr;
        qr.tapeMm = 24.0;
        qr.codeType = CodeType::Qr;
        qr.codeData = QStringLiteral("https://example.org");
        const Layout lqr = computeLayout(qr);
        check(!lqr.artwork.isNull(), "QR code is rendered");
        check(lqr.artwork.width() == lqr.artwork.height(), "QR code is square");
        checkNear(lqr.artworkWidth, qr.printablePt(), 1.0, "QR code fills the tape height");
    } else {
        std::printf("skipped: built without libqrencode\n");
    }
}

void testStatusDecoding()
{
    QByteArray frame(StatusLength, '\0');
    frame[10] = 12;      // tape width
    frame[11] = 0x01;    // laminated
    Status s = decodeStatus(frame);
    check(s.ok, "clean status counts as ready");
    check(s.tapeMm == 12, "tape width is read from byte 10");
    check(s.mediaType == QStringLiteral("laminated"), "media type is recognised");

    frame[8] = 0x01;     // no tape
    s = decodeStatus(frame);
    check(!s.ok, "an error bit invalidates the status");
    check(s.errors.contains(QStringLiteral("no tape inserted")), "error text recognised");

    s = decodeStatus(QByteArray(8, '\0'));
    check(!s.error.isEmpty(), "a short reply is rejected");
}


// Request and replies as they went over the wire to a PT-P950NW with 36 mm tape.
void testSnmp()
{
    const QString statusOid = QStringLiteral("1.3.6.1.4.1.2435.3.3.9.1.6.1.0");
    const QByteArray request = snmp::encodeGetRequest(statusOid, 0x12345678);
    check(request.toHex() == "302f02010004067075626c6963a02202041234567802010002010030143012"
                             "060e2b060104019303030309010601000500",
          "SNMP GET request is encoded byte for byte");
    check(snmp::encodeGetRequest(QStringLiteral("not.an.oid"), 1).isEmpty(),
          "an invalid OID is refused");

    const QByteArray statusReply = QByteArray::fromHex(
        "304f02010004067075626c6963a24202041234567802010002010030343032060e2b06010401"
        "93030303090106010004208020423070300400000024010000000000000000000000000108"
        "000000000000");
    snmp::Reply r = snmp::decodeResponse(statusReply);
    check(r.ok && r.requestId == 0x12345678, "SNMP reply decoded, request id recovered");
    check(r.tag == snmp::TagOctetString && r.value.size() == StatusLength,
          "the status value is the 32 byte block");
    const Status s = decodeStatus(r.value);
    check(s.ok && s.tapeMm == 36, "status over SNMP: ready, 36 mm tape");

    // noSuchName — what any device other than a P-touch says to the status OID.
    r = snmp::decodeResponse(QByteArray::fromHex(
        "302c02010004067075626c6963a21f0204123456780201020201013011300f060b2b0601040193"
        "03090909000500"));
    check(!r.ok && !r.error.isEmpty() && r.requestId == 0x12345678,
          "an SNMP error status is reported, not mistaken for a value");

    check(!snmp::decodeResponse(statusReply.left(40)).ok, "a truncated reply is rejected");
    check(!snmp::decodeResponse(QByteArray("garbage")).ok, "garbage is rejected");
}

void testNetworkDevice()
{
    check(isNetworkDevice(QStringLiteral("snmp://192.168.1.50")), "snmp://host is a network device");
    check(!isNetworkDevice(QStringLiteral("/dev/rfcomm0")), "a device node is not");
    check(!isNetworkDevice(QStringLiteral("snmp://")), "a scheme without a host is not");
    check(networkHost(networkDevice(QStringLiteral("labels.lan"))) == QStringLiteral("labels.lan"),
          "host survives the round trip");
    check(networkHost(QStringLiteral("/dev/usb/lp0")).isEmpty(), "a device node has no host");

    // What a CUPS queue prints to decides whether the printer is on the network.
    check(hostFromDeviceUri(QStringLiteral("socket://192.168.1.50:9100")) == QStringLiteral("192.168.1.50"),
          "host taken from a socket:// queue");
    check(hostFromDeviceUri(QStringLiteral("lpd://labels.lan/queue\n")) == QStringLiteral("labels.lan"),
          "host taken from an lpd:// queue");
    check(hostFromDeviceUri(QStringLiteral("usb://Brother/PT-P710BT?serial=1")).isEmpty(),
          "a USB queue has no host");
    check(hostFromDeviceUri(QStringLiteral("rfcomm:/dev/rfcomm0")).isEmpty(),
          "neither has the Bluetooth backend");

    // A printer that is switched off cannot name itself; its queue still does.
    check(modelFromQueueOptions(QStringLiteral(
              "copies=1 device-uri=socket://192.168.1.50:9100 "
              "printer-make-and-model='Brother PT-P950NW Foomatic/ptouch-pt (recommended)' "
              "printer-type=2101380")) == QStringLiteral("PT-P950NW"),
          "model taken from the queue's make-and-model");
    check(modelFromQueueOptions(QStringLiteral(
              "printer-make-and-model='HP LaserJet 500 color M551 Postscript'")).isEmpty(),
          "another make of printer has no P-touch model");
    check(modelFromQueueOptions(QStringLiteral("copies=1 finishings=3")).isEmpty(),
          "neither has a queue without the entry");
}

void testWideHeadModels()
{
    // The PT-P900 series has a 560 dot head at 360 dpi, the only one that reaches
    // across a 36 mm tape. Without the model a 128 dot head is assumed and the
    // page is clamped to 18 mm.
    for (const QString &name : {QStringLiteral("PT-P900W"), QStringLiteral("PT-P950NW"),
                                QStringLiteral("PT-P910BT")}) {
        const PrinterModel *m = modelFor(name);
        check(m && m->headDots == 560 && m->dpi == 360,
              QStringLiteral("%1 is known with its 560 dot head").arg(name));
    }
    Spec spec;
    spec.tapeMm = 36.0;
    spec.model = QStringLiteral("PT-P950NW");
    checkNear(spec.pagePt(), 102.0, 0.01, "PT-P950NW prints a 36 mm tape on a 102 pt page");
    checkNear(spec.printablePt(), 454 / 360.0 * 72.0, 0.01, "and reaches 454 dots of it");
}

void testCutterFeed()
{
    qunsetenv("PTOUCH_CUTTER_FEED_MM");
    check(cutterFeedMm(QStringLiteral("PT-P710BT")) == 5, "5 mm cutter feed by default");
    check(cutterFeedMm(QString()) == 5, "and for an unknown model");
    check(cutterFeedMm(QStringLiteral("PT-P950NW")) == 0, "none for the PT-P900 series");
    qputenv("PTOUCH_CUTTER_FEED_MM", "0");
    check(cutterFeedMm(QStringLiteral("PT-P710BT")) == 0, "the environment can switch it off");
    qputenv("PTOUCH_CUTTER_FEED_MM", "3");
    check(cutterFeedMm(QStringLiteral("PT-P950NW")) == 3, "or on");
    qputenv("PTOUCH_CUTTER_FEED_MM", "99");
    check(cutterFeedMm(QString()) == 5, "a value the driver does not offer is ignored");
    qputenv("PTOUCH_CUTTER_FEED_MM", "abc");
    check(cutterFeedMm(QString()) == 5, "and so is one that is not a number");
    qunsetenv("PTOUCH_CUTTER_FEED_MM");
}

void testTemplates()
{
    const QDateTime now(QDate(2026, 11, 10), QTime(9, 5, 7));
    const Row row{{QStringLiteral("name"), QStringLiteral("Switch A")},
                  {QStringLiteral("Asset ID"), QStringLiteral("NR-7")}};
    const auto fill = [&](const char *text, QStringList *unknown = nullptr) {
        return expandTemplate(QString::fromUtf8(text), row, 7, now, unknown);
    };
    check(fill("Rack {n}") == QStringLiteral("Rack 7"), "{n} is the counter");
    check(fill("{n:03}") == QStringLiteral("007"), "padded with zeros to a width");
    check(fill("{n:3}|") == QStringLiteral("  7|"), "or with blanks");
    check(fill("{date}") == QStringLiteral("2026-11-10"), "{date} is ISO by default");
    check(fill("{date:%d.%m.%Y}") == QStringLiteral("10.11.2026"), "strftime formats work");
    check(fill("{date:yyyy/MM}") == QStringLiteral("2026/11"), "and so do Qt formats");
    check(fill("{time}") == QStringLiteral("09:05"), "{time} is hours and minutes");
    check(fill("{time:%H:%M:%S}") == QStringLiteral("09:05:07"), "seconds on request");
    check(fill("{name} / {Asset ID}") == QStringLiteral("Switch A / NR-7"),
          "columns are filled by name, blanks in the name included");
    check(fill("{NAME} {asset id}") == QStringLiteral("Switch A NR-7"), "whatever the case");
    check(fill("{{n}} {n}") == QStringLiteral("{n} 7"), "doubled braces are literal");
    check(fill("a { b } c {") == QStringLiteral("a { b } c {"), "stray braces survive");

    QStringList unknown;
    check(fill("{nmae} {n}", &unknown) == QStringLiteral("{nmae} 7"),
          "an unknown field stays as written");
    check(unknown == QStringList{QStringLiteral("nmae")}, "and is reported");

    const Row shadow{{QStringLiteral("n"), QStringLiteral("column")},
                     {QStringLiteral("date"), QStringLiteral("someday")}};
    check(expandTemplate(QStringLiteral("{n} {date}"), shadow, 7, now) == QStringLiteral("column someday"),
          "a column wins over a built-in of the same name");
}

void testDataParsing()
{
    const DataSet csv = parseData(QByteArrayLiteral(
        "\xEF\xBB\xBFname,asset id,room\r\n"
        "Switch A,NR-0007,\"Rack 2, U14\"\r\n"
        "\r\n"
        "AP,NR-9,\"He said \"\"hi\"\"\nsecond line\"\n"));
    check(csv.error.isEmpty(), "CSV is read");
    check(csv.fields == QStringList({QStringLiteral("name"), QStringLiteral("asset id"),
                                     QStringLiteral("room")}),
          "the header names the fields, byte order mark aside");
    check(csv.rows.size() == 2, "blank lines are no labels");
    check(csv.rows.value(0).value(QStringLiteral("room")) == QStringLiteral("Rack 2, U14"),
          "quoted cells keep their commas");
    check(csv.rows.value(1).value(QStringLiteral("room")) == QStringLiteral("He said \"hi\"\nsecond line"),
          "and their quotes and line breaks");

    const DataSet semicolon = parseData(QByteArrayLiteral("a;b\n1,5;2\n"));
    check(semicolon.rows.value(0).value(QStringLiteral("a")) == QStringLiteral("1,5"),
          "semicolon-separated files are recognised");
    const DataSet tabs = parseData(QByteArrayLiteral("a\tb\nx\ty\n"));
    check(tabs.rows.value(0).value(QStringLiteral("b")) == QStringLiteral("y"), "so are tabs");
    check(!parseData(QByteArrayLiteral("a,b\n")).error.isEmpty(), "a header alone is an error");
    check(!parseData(QByteArray()).error.isEmpty(), "and so is nothing at all");

    const DataSet array = parseData(QByteArrayLiteral(
        "[{\"name\":\"alpha\",\"port\":1},{\"name\":\"beta\",\"port\":2.5,\"up\":true}]"));
    check(array.rows.size() == 2, "a JSON array gives one label per object");
    check(array.rows.value(0).value(QStringLiteral("port")) == QStringLiteral("1"),
          "whole numbers stay whole");
    check(array.rows.value(1).value(QStringLiteral("port")) == QStringLiteral("2.5")
              && array.rows.value(1).value(QStringLiteral("up")) == QStringLiteral("true"),
          "fractions and booleans are written out");
    const DataSet lines = parseData(QByteArrayLiteral("{\"a\":\"x\"}\n\n{\"a\":\"y\"}\n"));
    check(lines.rows.size() == 2 && lines.rows.value(1).value(QStringLiteral("a")) == QStringLiteral("y"),
          "one JSON object per line works too");
    check(!parseData(QByteArrayLiteral("[1,2]")).error.isEmpty(), "a list of numbers is refused");
    check(!parseData(QByteArrayLiteral("{\"a\":1}\n{oops\n")).error.isEmpty(),
          "and broken JSON names its line");
}

void testBatch(const QString &workDir)
{
    Spec spec;
    spec.tapeMm = 12;
    spec.autoSize = false;
    spec.sizePt = 10;
    spec.text = QStringLiteral("No. {n}");
    RunOptions run;
    run.start = 8;
    run.count = 4;                       // 8, 9, 10, 11
    Batch batch = buildBatch(spec, DataSet(), run);
    check(batch.size() == 4 && batch.errors.isEmpty(), "a count makes that many labels");
    check(batch.specs.value(0).text == QStringLiteral("No. 8")
              && batch.specs.value(3).text == QStringLiteral("No. 11"),
          "numbered from the start value");
    bool same = true;
    for (const Layout &l : std::as_const(batch.layouts))
        same &= std::abs(l.lengthPt - batch.lengthPt()) < 0.01 && !l.overflow;
    check(same, "all labels of a run share one length");
    const double alone = computeLayout(batch.specs.value(3)).lengthPt;
    Spec shortOne = spec;
    shortOne.text = QStringLiteral("No. 8");
    check(batch.lengthPt() > computeLayout(shortOne).lengthPt
              && std::abs(batch.lengthPt() - alone) < 0.01,
          "the longest label sets it");

    run.step = -2;
    check(buildBatch(spec, DataSet(), run).specs.value(1).text == QStringLiteral("No. 6"),
          "the step may count down");

    const QString path = workDir + QStringLiteral("/run.pdf");
    const PageSize page = writePdf(path, batch.specs, batch.layouts);
    QProcess info;
    info.start(QStringLiteral("pdfinfo"), {path});
    if (info.waitForFinished(5000) && info.exitCode() == 0) {
        const QString out = QString::fromLocal8Bit(info.readAllStandardOutput());
        check(out.contains(QRegularExpression(QStringLiteral("Pages:\\s+4\\b"))),
              "the PDF has one page per label");
    }
    check(page.widthPt == 34 && page.heightPt == int(qRound(batch.lengthPt())),
          "sized like the tape and the longest label");

    DataSet data = parseData(QByteArrayLiteral("name,id\nSwitch A,NR-7\nB,NR-10\n"));
    spec.text = QStringLiteral("{name}\n{id} #{n}");
    spec.codeType = CodeType::Code128;
    spec.codeData = QStringLiteral("{id}");
    spec.copies = 2;
    batch = buildBatch(spec, data, RunOptions());
    check(batch.size() == 4, "copies of a run repeat each label");
    check(batch.specs.value(0).text == QStringLiteral("Switch A\nNR-7 #1")
              && batch.specs.value(1).text == batch.specs.value(0).text
              && batch.specs.value(2).text == QStringLiteral("B\nNR-10 #2"),
          "label by label, rows numbered in order");
    check(batch.specs.value(2).codeData == QStringLiteral("NR-10"), "codes take fields as well");

    RunOptions firstOnly;
    firstOnly.count = 1;
    spec.copies = 1;
    check(buildBatch(spec, data, firstOnly).size() == 1, "a count limits the rows used");

    spec.text = QStringLiteral("{nmae}");
    batch = buildBatch(spec, data, RunOptions());
    check(!batch.errors.isEmpty() && batch.errors.first().contains(QStringLiteral("nmae")),
          "a column the data does not have is an error");
    check(buildBatch(spec, DataSet(), RunOptions()).errors.isEmpty(),
          "without data the braces are just text");
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QTemporaryDir dir;
    testTapeTable();
    testAutoSizeFillsTape();
    testMultiLineShrinks();
    testFixedLength();
    testOverflowDetected();
    testPdfGeometry(dir.path());
    testGlyphDetection();
    testArtwork();
    testStatusDecoding();
    testSnmp();
    testNetworkDevice();
    testWideHeadModels();
    testCutterFeed();
    testTemplates();
    testDataParsing();
    testBatch(dir.path());

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : qPrintable(QStringLiteral("%1 check(s) failed").arg(failures)));
    return failures == 0 ? 0 : 1;
}
