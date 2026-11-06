// Optional local harness for the same recursive engine used by the GUI.
#include "../common/fixedlayout.h"
#include "../common/recursivereplace.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>
static QString text(const RecursiveReplace::Path& path)
{
    QStringList p;
    for (int r : path)
        p << QString::number(r);
    return p.join('/');
}
static void list(const QByteArray& bytes, const TreeModel& m, const QModelIndex& root = {})
{
    for (int i = 0; i < m.rowCount(root); i++)
    {
        auto n = m.index(i, 0, root);
        if (m.type(n) == Types::File || m.type(n) == Types::Section)
        {
            const QString why = RecursiveReplace::eligibility(bytes, m, n);
            printf("%s %u %s %s %s\n", qPrintable(text(RecursiveReplace::path(n))), m.fullSize(n),
                qPrintable(m.name(n)), qPrintable(m.text(n)), why.isEmpty() ? "REPLACE" : qPrintable(why));
        }
        list(bytes, m, n);
    }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3 && argc != 6)
    {
        fprintf(stderr, "Usage: %s list IMAGE | body|asis IMAGE ROW/PATH REPLACEMENT NEW_OUTPUT\n", argv[0]);
        return 2;
    }
    QFile f(argv[2]);
    if (!f.open(QIODevice::ReadOnly))
        return 2;
    const QByteArray image = f.readAll();
    if (argc == 3 && QString(argv[1]) == "list")
    {
        TreeModel m;
        FfsParser p(&m);
        if (p.parse(image))
            return 1;
        list(image, m);
        return 0;
    }
    if (argc != 6 || (QString(argv[1]) != "body" && QString(argv[1]) != "asis"))
        return 2;
    RecursiveReplace::Path path;
    for (const QString& part : QString(argv[3]).split('/'))
    {
        bool ok;
        int r = part.toInt(&ok);
        if (!ok || r < 0)
            return 2;
        path << r;
    }
    QFile input(argv[4]);
    if (!input.open(QIODevice::ReadOnly))
        return 2;
    QByteArray out;
    QString error;
    if (!RecursiveReplace::replace(image, path, input.readAll(), QString(argv[1]) == "body", out, error)
        || !FixedLayout::saveAs(argv[2], argv[5], out, error))
    {
        fprintf(stderr, "Rejected: %s\n", qPrintable(error));
        return 1;
    }
    printf("Saved and reparsed %d bytes. Structural validation only; not a boot/authenticity guarantee.\n",
        out.size());
    return 0;
}
