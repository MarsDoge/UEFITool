// Experimental fixed-layout editor. BSD-2-Clause, as the rest of UEFITool.
#ifndef FIXEDLAYOUT_H
#define FIXEDLAYOUT_H
#include "ffsparser.h"
#include <QString>
#include <QByteArray>
#ifdef UEFITOOL_EDIT_TEST_HOOKS
#include <functional>
#endif

namespace FixedLayout {
#ifdef UEFITOOL_EDIT_TEST_HOOKS
// Synchronous regression hook: after staging, immediately before publication.
extern std::function<void(const QString&)> beforePublishForTest;
#endif
// These checks deliberately reject unsupported images rather than rebuilding them.
QString eligibility(const QByteArray& image, const TreeModel& model,
                    const FfsParser& parser, const QModelIndex& file);
bool replace(const QByteArray& image, quint32 offset, const QByteArray& replacement,
             bool bodyOnly, QByteArray& result, QString& error);
// Atomic Save As only. Existing destinations (including symlinks) are rejected.
bool saveAs(const QString& source, const QString& destination,
            const QByteArray& image, QString& error);
}
#endif
