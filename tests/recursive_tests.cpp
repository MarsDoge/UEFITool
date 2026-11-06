// Synthetic fixtures only. Optional argv[1] is a READ-ONLY local acceptance
// image; outputs go to argv[2]. No firmware images belong in this repository.
#include "../UEFITool/uefitool.h"
#include "../common/LZMA/LzmaCompress.h"
#include "../common/Tiano/EfiTianoCompress.h"
#include "../common/Tiano/EfiTianoDecompress.h"
#include "../common/ffs.h"
#include "../common/fixedlayout.h"
#include "../common/recursivereplace.h"
#include "../common/utility.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <cstdio>
#include <cstdlib>
static int checks = 0;
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        ++checks;                                                                                            \
        if (!(x))                                                                                            \
        {                                                                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void put16(QByteArray& a, int p, quint16 v)
{
    qToLittleEndian(v, reinterpret_cast<uchar*>(a.data() + p));
}
static void put32(QByteArray& a, int p, quint32 v)
{
    qToLittleEndian(v, reinterpret_cast<uchar*>(a.data() + p));
}
static void put64(QByteArray& a, int p, quint64 v)
{
    qToLittleEndian(v, reinterpret_cast<uchar*>(a.data() + p));
}
static void put24(QByteArray& a, int p, int v)
{
    for (int i = 0; i < 3; i++)
        a[p + i] = char(v >> (i * 8));
}
static int aligned(int n, int a)
{
    return (n + a - 1) & ~(a - 1);
}
static QByteArray section(const QByteArray& b, int type = EFI_SECTION_RAW, bool extended = false)
{
    QByteArray h(extended ? 8 : 4, 0);
    h[3] = char(type);
    put24(h, 0, extended ? 0xffffff : h.size() + b.size());
    if (extended)
        put32(h, 4, h.size() + b.size());
    return h + b;
}
static QByteArray file(const QByteArray& b, int type = EFI_FV_FILETYPE_FREEFORM, int id = 1, int attr = 0x40,
    bool polarity = true, bool large = false)
{
    QByteArray h(large ? 32 : 24, 0);
    h.replace(0, 16, QByteArray::fromHex("102030405060708090a0b0c0d0e0f000"));
    h[0] = char(id);
    h[18] = char(type);
    h[19] = char(attr | (large ? 1 : 0));
    put24(h, 20, large ? 0xffffff : h.size() + b.size());
    if (large)
        put64(h, 24, h.size() + b.size());
    h[16] = char(calculateChecksum8(reinterpret_cast<const UINT8*>(h.constData()), h.size()));
    h[17] = attr & 0x40 ? char(calculateChecksum8(reinterpret_cast<const UINT8*>(b.constData()), b.size()))
                        : char(0xaa);
    h[23] = polarity ? char(0xf8) : 7;
    return h + b;
}
static QByteArray volume(
    const QList<QByteArray>& files, int size = 4096, bool v3 = false, bool polarity = true)
{
    QByteArray h(72, 0);
    h.replace(16, 16, v3 ? EFI_FIRMWARE_FILE_SYSTEM3_GUID : EFI_FIRMWARE_FILE_SYSTEM2_GUID);
    put64(h, 32, size);
    put32(h, 40, EFI_FV_SIGNATURE);
    put32(h, 44, (polarity ? EFI_FVB2_ERASE_POLARITY : 0) | 0x00070000);
    put16(h, 48, 72);
    h[55] = 2;
    put32(h, 56, 1);
    put32(h, 60, size);
    put16(h, 50, calculateChecksum16(reinterpret_cast<const UINT16*>(h.constData()), h.size()));
    for (auto f : files)
    {
        h += QByteArray(aligned(h.size(), 8) - h.size(), polarity ? char(0xff) : char(0));
        h += f;
    }
    CHECK(h.size() <= size);
    h += QByteArray(size - h.size(), polarity ? char(0xff) : char(0));
    return h;
}
static QByteArray compressed(const QByteArray& data, int a, bool guided = false)
{
    QByteArray encoded;
    UINT32 size = 0;
    if (a == COMPRESSION_ALGORITHM_NONE)
        encoded = data;
    else
    {
        if (a == COMPRESSION_ALGORITHM_LZMA)
            CHECK(LzmaCompress(
                      reinterpret_cast<const UINT8*>(data.constData()), data.size(), nullptr, &size, 1 << 20)
                == U_BUFFER_TOO_SMALL);
        else
            CHECK((a == COMPRESSION_ALGORITHM_EFI11 ? EfiCompress : TianoCompress)(
                      data.constData(), data.size(), nullptr, &size)
                == U_BUFFER_TOO_SMALL);
        encoded.resize(size);
        if (a == COMPRESSION_ALGORITHM_LZMA)
            CHECK(LzmaCompress(reinterpret_cast<const UINT8*>(data.constData()), data.size(),
                      reinterpret_cast<UINT8*>(encoded.data()), &size, 1 << 20)
                == U_SUCCESS);
        else
            CHECK((a == COMPRESSION_ALGORITHM_EFI11 ? EfiCompress : TianoCompress)(
                      data.constData(), data.size(), encoded.data(), &size)
                == U_SUCCESS);
        encoded.resize(size);
    }
    if (guided)
    {
        QByteArray h(24, 0);
        h[3] = EFI_SECTION_GUID_DEFINED;
        h.replace(
            4, 16, a == COMPRESSION_ALGORITHM_LZMA ? EFI_GUIDED_SECTION_LZMA : EFI_GUIDED_SECTION_TIANO);
        put16(h, 20, 24);
        put16(h, 22, 1);
        put24(h, 0, h.size() + encoded.size());
        return h + encoded;
    }
    QByteArray h(9, 0);
    h[3] = EFI_SECTION_COMPRESSION;
    put32(h, 4, data.size());
    h[8] = a == COMPRESSION_ALGORITHM_NONE ? 0 : a == COMPRESSION_ALGORITHM_LZMA ? 2 : 1;
    put24(h, 0, h.size() + encoded.size());
    return h + encoded;
}
static QByteArray crcSection(const QByteArray& b)
{
    QByteArray h(28, 0);
    h[3] = EFI_SECTION_GUID_DEFINED;
    h.replace(4, 16, EFI_GUIDED_SECTION_CRC32);
    put16(h, 20, 28);
    put16(h, 22, 2);
    put32(h, 24, crc32(0, reinterpret_cast<const Bytef*>(b.constData()), b.size()));
    put24(h, 0, 28 + b.size());
    return h + b;
}
static QByteArray read(const QString& p)
{
    QFile f(p);
    CHECK(f.open(QIODevice::ReadOnly));
    return f.readAll();
}
static void write(const QString& p, const QByteArray& b)
{
    QFile f(p);
    CHECK(f.open(QIODevice::WriteOnly));
    CHECK(f.write(b) == b.size());
}
static QModelIndex first(const TreeModel& m, int type, int sub = -1, QModelIndex root = {})
{
    for (int i = 0; i < m.rowCount(root); i++)
    {
        auto n = m.index(i, 0, root);
        if (m.type(n) == type && (sub < 0 || m.subtype(n) == sub))
            return n;
        auto f = first(m, type, sub, n);
        if (f.isValid())
            return f;
    }
    return {};
}
static QModelIndex named(const TreeModel& m, const QString& name, QModelIndex root = {})
{
    for (int i = 0; i < m.rowCount(root); i++)
    {
        auto n = m.index(i, 0, root);
        if (m.type(n) == Types::File && m.text(n) == name)
            return n;
        auto f = named(m, name, n);
        if (f.isValid())
            return f;
    }
    return {};
}
static QByteArray apply(
    const QByteArray& a, const RecursiveReplace::Path& p, const QByteArray& data, bool body = true)
{
    QString e;
    QByteArray out;
    bool ok = RecursiveReplace::replace(a, p, data, body, out, e);
    if (!ok)
        fprintf(stderr, "replace error: %s\n", qPrintable(e));
    CHECK(ok);
    CHECK(out.size() == a.size());
    return out;
}
static void reject(
    const QByteArray& a, const RecursiveReplace::Path& p, const QByteArray& data, bool body = true)
{
    QString e;
    QByteArray out("unchanged");
    CHECK(!RecursiveReplace::replace(a, p, data, body, out, e));
    CHECK(!e.isEmpty());
    CHECK(out == "unchanged");
    fprintf(stderr, "Expected refusal: %s\n", qPrintable(e));
}
static void menu(QAction* action, const QString& path)
{
    QTimer timer;
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout,
        [&]()
        {
            CHECK(++ticks < 200);
            for (QWidget* w : QApplication::topLevelWidgets())
            {
                if (!w->isVisible())
                    continue;
                if (auto d = qobject_cast<QFileDialog*>(w))
                {
                    auto line = d->findChild<QLineEdit*>("fileNameEdit");
                    CHECK(line);
                    line->setText(path);
                    QMetaObject::invokeMethod(d, "accept", Qt::DirectConnection);
                    return;
                }
                if (auto box = qobject_cast<QMessageBox*>(w))
                {
                    fprintf(stderr, "GUI: %s\n", qPrintable(box->text()));
                    CHECK(box->standardButtons() & QMessageBox::Yes);
                    box->button(QMessageBox::Yes)->click();
                    return;
                }
            }
        });
    timer.start(20);
    action->trigger();
    timer.stop();
}
static void gui(const QString& source, const QString& replacement, const QString& output,
    const RecursiveReplace::Path& p, bool body)
{
    UEFITool w;
    w.resize(1280, 800);
    w.show();
    w.openImageFile(source);
    auto tree = w.findChild<QTreeView*>("structureTreeView");
    CHECK(tree);
    auto m = static_cast<TreeModel*>(tree->model());
    auto n = RecursiveReplace::index(*m, p);
    CHECK(n.isValid());
    const int selectedType = m->type(n);
    tree->setCurrentIndex(n);
    auto action = w.findChild<QAction*>(body ? "actionReplaceBody" : "actionReplace");
    CHECK(action && action->isEnabled());
    menu(action, replacement);
    CHECK(w.hasUnsavedChanges());
    m = static_cast<TreeModel*>(tree->model());
    CHECK(tree->currentIndex().isValid());
    CHECK(m->type(tree->currentIndex()) == selectedType);
    for (auto parent = tree->currentIndex().parent(); parent.isValid(); parent = parent.parent())
        tree->expand(parent);
    tree->scrollTo(tree->currentIndex());
    QApplication::processEvents();
    CHECK(w.grab().save(output + ".png"));
    auto save = w.findChild<QAction*>("actionSaveImageFile");
    CHECK(save && save->isEnabled());
    menu(save, output);
    CHECK(!w.hasUnsavedChanges());
    CHECK(w.close());
    TreeModel checked;
    FfsParser parser(&checked);
    CHECK(parser.parse(read(output)) == U_SUCCESS);
}
int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("UEFIToolSyntheticTests");
    app.setApplicationName("RecursiveReplaceTests");
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString error;
    const QByteArray leaf = section(QByteArray(32, 'A'));
    const QByteArray sibling = file(section("opaque sibling"), EFI_FV_FILETYPE_FREEFORM, 2);
    QByteArray a = volume({ file(leaf), sibling });
    TreeModel m;
    FfsParser parser(&m);
    CHECK(parser.parse(a) == U_SUCCESS);
    auto f = first(m, Types::File);
    auto s = first(m, Types::Section);
    auto fp = RecursiveReplace::path(f), sp = RecursiveReplace::path(s);
    CHECK(apply(a, fp, m.full(f), false) == a);
    CHECK(apply(a, sp, m.body(s)) == a);
    for (int length : { 1, 3, 4, 17, 32, 33, 128, 1024 })
    {
        QByteArray payload(length, 'Z');
        auto out = apply(a, sp, payload);
        TreeModel c;
        FfsParser p(&c);
        CHECK(p.parse(out) == U_SUCCESS);
        CHECK(c.body(first(c, Types::Section)) == payload);
        CHECK(out.contains(sibling));
        CHECK(apply(a, sp, section(payload), false) == out);
        auto ff = apply(a, fp, section(payload));
        CHECK(ff == out);
        CHECK(apply(a, fp, file(section(payload)), false) == out);
    }
    reject(a, sp, QByteArray(5000, 'Z'));
    reject(a, sp, "bad", false);
    reject(a, fp, file(leaf, EFI_FV_FILETYPE_RAW), false);
    QByteArray malformed = leaf;
    malformed[0] = char(100);
    reject(a, sp, malformed, false);
    QByteArray bad = a;
    bad[50] = bad.at(50) ^ 1;
    reject(bad, sp, "B");
    bad = a;
    bad[88] = bad.at(88) ^ 1;
    reject(bad, sp, "B");
    // Compression at two depths, all supported standard algorithms, with CRC.
    for (int alg : { COMPRESSION_ALGORITHM_NONE, COMPRESSION_ALGORITHM_EFI11, COMPRESSION_ALGORITHM_TIANO,
             COMPRESSION_ALGORITHM_LZMA })
    {
        a = volume({ file(compressed(crcSection(compressed(leaf, alg)), alg)), sibling }, 8192);
        TreeModel c;
        FfsParser p(&c);
        CHECK(p.parse(a) == U_SUCCESS);
        auto n = first(c, Types::Section, EFI_SECTION_RAW);
        CHECK(n.isValid());
        auto path = RecursiveReplace::path(n);
        CHECK(apply(a, path, c.body(n)) == a);
        for (int length : { 9, 79 })
        {
            auto out = apply(a, path, QByteArray(length, 'K'));
            TreeModel v;
            FfsParser vp(&v);
            CHECK(vp.parse(out) == U_SUCCESS);
            CHECK(v.body(first(v, Types::Section, EFI_SECTION_RAW)) == QByteArray(length, 'K'));
        }
        // Legacy Replace body on the container takes decoded bytes, not a
        // serialized compressed stream. As-is takes the full serialized section.
        auto container = first(c, Types::Section, EFI_SECTION_COMPRESSION);
        auto containerPath = RecursiveReplace::path(container);
        CHECK(apply(a, containerPath, RecursiveReplace::replacementBody(c, container)) == a);
        const auto stream = section(QByteArray(37, 'D'));
        auto decodedEdit = apply(a, containerPath, stream);
        CHECK(apply(a, containerPath, compressed(stream, alg), false) == decodedEdit);
    }
    for (int alg : { COMPRESSION_ALGORITHM_LZMA, COMPRESSION_ALGORITHM_TIANO })
    {
        auto inner = volume({ file(leaf), sibling }, 4096);
        a = volume({ file(compressed(section(inner, EFI_SECTION_FIRMWARE_VOLUME_IMAGE, true), alg, true),
                       EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE) },
            8192, true);
        TreeModel c;
        FfsParser p(&c);
        CHECK(p.parse(a) == U_SUCCESS);
        auto n = first(c, Types::Section, EFI_SECTION_RAW);
        CHECK(n.isValid());
        auto path = RecursiveReplace::path(n);
        auto out = apply(a, path, QByteArray(500, 'N'));
        CHECK(out != a);
        reject(a, path, QByteArray(5000, 'O'));
    }
    // EFI-only primary output must not be mistaken for Tiano. Check with
    // the intended decoder directly: NE's dual-decoder reparse masks this bug.
    for (int alg : { COMPRESSION_ALGORITHM_EFI11, COMPRESSION_ALGORITHM_TIANO })
    {
        QByteArray payload(32, 0);
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = char('A' + i % 17);
        const auto originalStream = section(payload);
        const auto wrapped = compressed(originalStream, alg, true);
        if (alg == COMPRESSION_ALGORITHM_EFI11)
        {
            UINT8 detected = 0;
            UINT32 dictionary = 0;
            QByteArray primary, alternate;
            CHECK(decompress(wrapped.mid(24), EFI_STANDARD_COMPRESSION, detected, dictionary,
                      primary, alternate) == U_SUCCESS);
            CHECK(detected == COMPRESSION_ALGORITHM_EFI11);
            CHECK(primary == originalStream);
        }
        auto strictDecode = [alg](const QByteArray& encoded, const QByteArray& expected)
        {
            UINT32 size = 0, scratchSize = 0;
            CHECK(EfiTianoGetInfo(encoded.constData(), encoded.size(), &size, &scratchSize) == U_SUCCESS);
            CHECK(size == quint32(expected.size()));
            QByteArray decoded(size, 0), scratch(scratchSize, 0);
            CHECK((alg == COMPRESSION_ALGORITHM_EFI11 ? EfiDecompress : TianoDecompress)(
                      encoded.constData(), encoded.size(), decoded.data(), size, scratch.data(), scratchSize)
                == U_SUCCESS);
            CHECK(decoded == expected);
        };
        strictDecode(wrapped.mid(24), originalStream);
        const auto image = volume({ file(wrapped), sibling }, 16384);
        TreeModel original;
        FfsParser originalParser(&original);
        CHECK(originalParser.parse(image) == U_SUCCESS);
        auto raw = first(original, Types::Section, EFI_SECTION_RAW);
        auto guided = first(original, Types::Section, EFI_SECTION_GUID_DEFINED);
        CHECK(raw.isValid() && guided.isValid());
        payload[0] = char(payload.at(0) ^ 1);
        // Exercise both ancestor rebuilding and direct decoded-body replacement.
        for (bool container : { false, true })
        {
            const auto output = apply(image, RecursiveReplace::path(container ? guided : raw),
                container ? section(payload) : payload);
            CHECK(output != image);
            CHECK(output.contains(sibling));
            TreeModel fresh;
            FfsParser freshParser(&fresh);
            CHECK(freshParser.parse(output) == U_SUCCESS);
            const auto rebuilt = first(fresh, Types::Section, EFI_SECTION_GUID_DEFINED);
            CHECK(rebuilt.isValid());
            CHECK(fresh.header(rebuilt).mid(4, 20) == original.header(guided).mid(4, 20));
            strictDecode(fresh.body(rebuilt), section(payload));
        }
    }
    // Physical overflow after recompression is distinct from nested overflow.
    auto nested = volume({ file(leaf) }, 8192);
    auto packed = file(
        compressed(section(nested, EFI_SECTION_FIRMWARE_VOLUME_IMAGE), COMPRESSION_ALGORITHM_LZMA, true),
        EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE);
    a = volume({ packed }, aligned(72 + packed.size() + 32, 8));
    TreeModel c;
    FfsParser cp(&c);
    CHECK(cp.parse(a) == U_SUCCESS);
    auto nestedPath = RecursiveReplace::path(first(c, Types::Section, EFI_SECTION_RAW));
    QByteArray random(3000, 0);
    quint32 state = 12345;
    for (int i = 0; i < random.size(); i++)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        random[i] = char(state);
    }
    reject(a, nestedPath, random);
    // Fixed / early-boot file relocation is refused, opaque sibling retained.
    a = volume({ file(leaf), file(leaf, EFI_FV_FILETYPE_PEIM, 2) });
    reject(a, sp, QByteArray(200, 'P'));
    a = volume({ file(leaf, EFI_FV_FILETYPE_FREEFORM, 1, 0x44) });
    reject(a, sp, "P");
    // Alignment padding is regenerated as valid PAD FFS, not a free-space hole.
    auto pad = file(QByteArray(72, char(0xff)), EFI_FV_FILETYPE_PAD, 9, 0);
    auto alignedFile = file(leaf, EFI_FV_FILETYPE_FREEFORM, 2, 0x10);
    a = volume({ file(leaf), pad, alignedFile });
    auto alignedOut = apply(a, sp, QByteArray(80, 'X'));
    CHECK(alignedOut.contains(alignedFile));
    TreeModel am;
    FfsParser ap(&am);
    CHECK(ap.parse(alignedOut) == U_SUCCESS);
    for (const auto& msg : ap.getMessages())
        CHECK(!msg.first.contains("alignment"));
    // Opaque non-empty PAD siblings may not be silently dropped or relocated.
    auto opaquePad = file(QByteArray(72, '?'), EFI_FV_FILETYPE_PAD, 9, 0);
    a = volume({ file(leaf), opaquePad, sibling });
    reject(a, sp, QByteArray(80, 'X'));
    CHECK(apply(a, sp, QByteArray(32, 'X')).contains(opaquePad));
    // CRC-guided section body replacement recomputes CRC, including as-is.
    a = volume({ file(crcSection(leaf)), sibling });
    TreeModel cr;
    FfsParser crp(&cr);
    CHECK(crp.parse(a) == U_SUCCESS);
    auto crPath = RecursiveReplace::path(first(cr, Types::Section, EFI_SECTION_GUID_DEFINED));
    auto newStream = section("CRC replacement");
    CHECK(apply(a, crPath, newStream) == apply(a, crPath, crcSection(newStream), false));
    // Coloring/security marking is not a blanket prohibition.
    cr.setMarking(first(cr, Types::File), 1);
    cr.setMarkingEnabled(false);
    CHECK(RecursiveReplace::eligibility(a, cr, first(cr, Types::File)).isEmpty());
    // Unknown guided codec is an explicit refusal, not a parser fallback.
    auto unknown = crcSection(leaf);
    unknown[4] = char(0xab);
    a = volume({ file(unknown) });
    TreeModel un;
    FfsParser up(&un);
    CHECK(up.parse(a) == U_SUCCESS);
    reject(a, RecursiveReplace::path(first(un, Types::Section, EFI_SECTION_GUID_DEFINED)), leaf);
    // Existing extended FFS3 headers remain valid; FFS2 large files rejected.
    a = volume({ file(leaf, EFI_FV_FILETYPE_FREEFORM, 1, 0x40, true, true) }, 4096, true);
    TreeModel l;
    FfsParser lp(&l);
    CHECK(lp.parse(a) == U_SUCCESS);
    CHECK(!apply(a, RecursiveReplace::path(first(l, Types::Section)), "large header").isEmpty());
    // Actual 24-bit crossings: promote both common section and FFS headers in
    // FFS3; refuse rather than truncate in FFS2.
    QByteArray largeBody(0x1000000, 'L');
    a = volume({ file(leaf) }, 0x1100000, true);
    TreeModel big;
    FfsParser bp(&big);
    CHECK(bp.parse(a) == U_SUCCESS);
    auto bigPath = RecursiveReplace::path(first(big, Types::Section));
    auto largeOut = apply(a, bigPath, largeBody);
    TreeModel bigCheck;
    FfsParser bcp(&bigCheck);
    CHECK(bcp.parse(largeOut) == U_SUCCESS);
    CHECK(bigCheck.headerSize(first(bigCheck, Types::File)) == 32);
    CHECK(bigCheck.headerSize(first(bigCheck, Types::Section)) == 8);
    a = volume({ file(leaf) }, 0x1100000, false);
    reject(a, bigPath, largeBody);
    // GUI drives real menu slots, dialogs, confirmation and atomic Save As.
    a = volume({ file(compressed(leaf, COMPRESSION_ALGORITHM_LZMA)), sibling });
    TreeModel g;
    FfsParser gp(&g);
    CHECK(gp.parse(a) == U_SUCCESS);
    const QString src = dir.path() + "/synthetic.fd", repl = dir.path() + "/body.bin",
                  dst = dir.path() + "/gui.fd";
    write(src, a);
    write(repl, QByteArray(123, 'G'));
    gui(src, repl, dst, RecursiveReplace::path(first(g, Types::Section, EFI_SECTION_RAW)), true);
    CHECK(read(src) == a);
    if (argc == 3)
    {
        const QString source = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath(),
                      outputDir = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
        CHECK(QDir(outputDir).exists());
        const QByteArray original = read(source);
        const auto hash = QCryptographicHash::hash(original, QCryptographicHash::Sha256);
        TreeModel real;
        FfsParser rp(&real);
        CHECK(rp.parse(original) == U_SUCCESS);
        auto target = named(real, "DebugPortProtocolDxe");
        CHECK(target.isValid());
        auto path = RecursiveReplace::path(target);
        fprintf(stderr, "REAL target=%s size=%u compressed=%d\n", qPrintable(real.name(target)),
            real.fullSize(target), real.compressed(target));
        CHECK(RecursiveReplace::eligibility(original, real, target).isEmpty());
        CHECK(apply(original, path, real.full(target), false) == original);
        QByteArray body = real.body(target);
        body += QByteArray(aligned(body.size(), 4) - body.size(), 0);
        body += section("RecursiveReplace local acceptance fixture");
        const QByteArray changed = apply(original, path, body);
        const QString engineOut = outputDir + "/recursive-real-engine.fd";
        CHECK(FixedLayout::saveAs(source, engineOut, changed, error));
        CHECK(read(engineOut) == changed);
        TreeModel check;
        FfsParser vp(&check);
        CHECK(vp.parse(read(engineOut)) == U_SUCCESS);
        auto t = named(check, "DebugPortProtocolDxe");
        CHECK(t.isValid());
        CHECK(check.body(t) == body);
        const QString replacementPath = outputDir + "/recursive-real-replacement.ffs";
        write(replacementPath, check.full(t));
        gui(source, replacementPath, outputDir + "/recursive-real-gui.fd", path, false);
        CHECK(read(outputDir + "/recursive-real-gui.fd") == changed);
        // A real nested UI section grows through the same full GUI workflow.
        auto ui = first(real, Types::Section, EFI_SECTION_USER_INTERFACE, target);
        CHECK(ui.isValid());
        QByteArray uiBody = real.body(ui);
        CHECK(uiBody.endsWith(QByteArray(2, 0)));
        uiBody.chop(2);
        uiBody += QByteArray::fromHex("20002b000000");
        const QString uiReplacement = outputDir + "/recursive-real-ui.sbd";
        write(uiReplacement, uiBody);
        gui(source, uiReplacement, outputDir + "/recursive-real-section-gui.fd", RecursiveReplace::path(ui),
            true);
        CHECK(read(outputDir + "/recursive-real-section-gui.fd")
            == apply(original, RecursiveReplace::path(ui), uiBody));
        // Shrink through the same nested codec path back to the extracted body.
        CHECK(apply(changed, RecursiveReplace::path(t), real.body(target)).size() == original.size());
        CHECK(QCryptographicHash::hash(read(source), QCryptographicHash::Sha256) == hash);
        fprintf(stderr, "REAL SHA256 unchanged: %s\n", hash.toHex().constData());
    }
    printf("PASS: %d recursive checks (synthetic + real Qt menu workflow%s)\n", checks,
        argc == 3 ? " + read-only real fixture" : "");
    return 0;
}
