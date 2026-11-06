// Transactional NE-tree replacement. BSD-2-Clause.
#ifndef RECURSIVEREPLACE_H
#define RECURSIVEREPLACE_H
#include "ffsparser.h"
#include <QStringList>
#include <QVector>

namespace RecursiveReplace
{
// Rows identify a node even inside decompressed buffers, where base is not a
// physical image offset. Paths are only valid for the image they came from.
using Path = QVector<int>;
Path path(const QModelIndex& node);
QModelIndex index(const TreeModel& model, const Path& path);
QString eligibility(const QByteArray& image, const TreeModel& model, const QModelIndex& node);
// Legacy Replace body semantics: decoded data for a locally compressed section;
// otherwise the serialized body. NE's Extract uncompressed supplies decoded data.
QByteArray replacementBody(const TreeModel& model, const QModelIndex& node);
QStringList warnings(const FfsParser& parser);
bool replace(const QByteArray& image, const Path& target, const QByteArray& replacement, bool bodyOnly,
    QByteArray& result, QString& error, Path* resultPath = nullptr);
}
#endif
