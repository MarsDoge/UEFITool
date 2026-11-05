// Experimental fixed-layout editor. BSD-2-Clause.
#include "fixedlayout.h"
#include "ffs.h"
#include <QFileInfo>
#include <QDir>
#include <QTemporaryFile>
#include <QtEndian>
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <cerrno>
#include <cstring>
#elif defined(Q_OS_WIN)
// basetypes.h defines VOID, but winnt.h uses it to guard CHAR/SHORT/LONG
// typedefs as well. Let the SDK supply that block (and redefine VOID).
#undef VOID
#include <qt_windows.h>
#endif

#ifdef UEFITOOL_EDIT_TEST_HOOKS
std::function<void(const QString&)> FixedLayout::beforePublishForTest;
#endif

namespace {
struct StagedFileCleanup {
    QString path;
    ~StagedFileCleanup() { if(!path.isEmpty()) QFile::remove(path); }
};
quint8 readByte(const QByteArray& a, int n) { return static_cast<quint8>(a.at(n)); }
quint16 word(const QByteArray& a, int n) { return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(a.constData()+n)); }
quint32 dword(const QByteArray& a, int n) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(a.constData()+n)); }
quint64 qword(const QByteArray& a, int n) { return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(a.constData()+n)); }
int sum8(const QByteArray& a) { quint8 s = 0; for (char c : a) s += static_cast<quint8>(c); return s; }
bool within(const QModelIndex& node, const QModelIndex& parent) {
    for (QModelIndex p = node; p.isValid(); p = p.parent()) if (p == parent) return true;
    return false;
}
bool supportedTree(const TreeModel& m, const QModelIndex& n) {
    if (m.compressed(n) || m.marking(n)) return false;
    if (m.type(n) == Types::Section) {
        // Do not invalidate an embedded PE Authenticode certificate. NE's
        // firmware protection metadata does not cover PE security directories.
        if (m.subtype(n) == EFI_SECTION_PE32) {
            const QByteArray pe = m.body(n);
            if (pe.size()<64) return false;
            const quint32 nt = pe.startsWith("MZ") ? dword(pe,60) : 0;
            if (nt>quint32(pe.size()-26) || pe.mid(int(nt),4)!=QByteArray("PE\0\0",4)) return false;
            const int opt = int(nt)+24;
            const int dir = word(pe,opt)==0x10b ? 96 : word(pe,opt)==0x20b ? 112 : 0;
            const int optSize = word(pe,int(nt)+20);
            if (!dir || optSize<dir || optSize>pe.size()-opt) return false;
            const quint32 count = dword(pe,opt+dir-4);
            if (count>quint32((optSize-dir)/8)) return false;
            if (count>4 && (dword(pe,opt+dir+32) || dword(pe,opt+dir+36))) return false;
        }
        switch (m.subtype(n)) {
        case EFI_SECTION_PE32: case EFI_SECTION_TE: case EFI_SECTION_RAW:
        case EFI_SECTION_USER_INTERFACE: case EFI_SECTION_VERSION:
        case EFI_SECTION_DXE_DEPEX: case EFI_SECTION_PEI_DEPEX: case EFI_SECTION_MM_DEPEX: break;
        default: return false;
        }
    } else if (m.type(n) != Types::File) return false;
    for (int i=0; i<m.rowCount(n); ++i) if (!supportedTree(m, m.index(i,0,n))) return false;
    return true;
}
bool sameLayout(const TreeModel& a, const QModelIndex& ai, const TreeModel& b, const QModelIndex& bi) {
    if (a.type(ai)!=b.type(bi) || a.subtype(ai)!=b.subtype(bi) ||
        a.base(ai)!=b.base(bi) || a.fullSize(ai)!=b.fullSize(bi) ||
        a.headerSize(ai)!=b.headerSize(bi) || a.tailSize(ai)!=b.tailSize(bi) ||
        a.rowCount(ai)!=b.rowCount(bi)) return false;
    for(int i=0;i<a.rowCount(ai);++i)
        if(!sameLayout(a,a.index(i,0,ai),b,b.index(i,0,bi))) return false;
    return true;
}
QString fileCheck(const QByteArray& f, bool polarity, quint32 relative) {
    if (f.size()<24 || (relative & 7)) return "FFS header is truncated or not 8-byte aligned.";
    const int size = readByte(f,20) | readByte(f,21)<<8 | readByte(f,22)<<16;
    const quint8 attr = readByte(f,19);
    if (size != f.size() || (attr & (0x80 | FFS_ATTRIB_LARGE_FILE | FFS_ATTRIB_DATA_ALIGNMENT2)))
        return "Large files, extended alignment, reserved attributes or inconsistent sizes are unsupported.";
    if (readByte(f,18)<1 || readByte(f,18)>15 || readByte(f,18)==EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE)
        return "Unsupported file type.";
    if (readByte(f,23) != (polarity ? 0xF8 : 0x07)) return "Only DATA_VALID files with canonical state are editable.";
    const unsigned alignment = 1U << ffsAlignmentTable[(attr & FFS_ATTRIB_DATA_ALIGNMENT)>>3];
    if ((relative+24) % alignment) return "Invalid FFS data alignment.";
    QByteArray h = f.left(24); h[17]=0; h[23]=0;
    if (sum8(h)) return "Invalid FFS header checksum.";
    if (attr & FFS_ATTRIB_CHECKSUM) {
        if ((sum8(f.mid(24)) + readByte(f,17)) % 256) return "Invalid FFS data checksum.";
    } else if (readByte(f,17) != FFS_FIXED_CHECKSUM2) return "Invalid FFS fixed checksum.";
    return QString();
}
}

QString FixedLayout::eligibility(const QByteArray& image, const TreeModel& m,
                                const FfsParser& parser, const QModelIndex& file) {
    if (!file.isValid() || m.type(file)!=Types::File) return "Select a physical FFS file.";
    if (parser.hasProtectedRanges() || !parser.getSecurityInfo().isEmpty())
        return "Images with recognized protection/security metadata are read-only.";
    const QModelIndex volume = file.parent();
    if (m.type(volume)!=Types::Volume) return "File must be directly inside a standard firmware volume.";
    for (QModelIndex p=volume.parent();p.isValid();p=p.parent()) {
        if ((m.type(p)!=Types::Image && m.type(p)!=Types::Region) || m.compressed(p) || m.marking(p))
            return "Nested volumes, capsules, compressed or authenticated containers are unsupported.";
    }
    if (m.compressed(volume) || m.marking(volume) || !supportedTree(m,file))
        return "Protected, compressed, GUID-defined, nested or unsupported file contents are read-only.";
    const QByteArray v = m.full(volume);
    if (v.size()<72 || dword(v,40)!=EFI_FV_SIGNATURE || readByte(v,55)!=2 || readByte(v,54)!=0)
        return "Only revision-2 standard volumes are supported.";
    if (v.mid(16,16)!=EFI_FIRMWARE_FILE_SYSTEM2_GUID && v.mid(16,16)!=EFI_FIRMWARE_FILE_SYSTEM3_GUID)
        return "Nonstandard or authenticated volume filesystem is unsupported.";
    const int hsize = word(v,48);
    if(hsize<72 || (hsize&7) || hsize>v.size() || qword(v,32)!=quint64(v.size())) return "Invalid volume length/header alignment.";
    const int ext = word(v,52);
    // A name-only extension carries no authentication or mutable UsedSize data.
    if (ext && (ext<hsize || ext>v.size()-20 || dword(v,ext+16)!=20 ||
                quint32(ext+20)>m.headerSize(volume)))
        return "Only name-only standard FV extended headers are supported.";
    quint16 sum=0; for(int i=0;i<hsize;i+=2) sum+=word(v,i);
    if(sum) return "Invalid volume header checksum.";
    quint64 mapped=0; bool end=false;
    for(int i=56;i+8<=hsize;i+=8) {
        const quint32 count=dword(v,i), length=dword(v,i+4);
        if(!count && !length) { end=true; break; }
        if(!count || !length || quint64(count)*length > quint64(v.size())-mapped) return "Invalid volume block map.";
        mapped+=quint64(count)*length;
    }
    if(!end || mapped!=quint64(v.size())) return "Invalid volume block map extent.";
    const quint64 base=m.base(file), size=m.fullSize(file), vb=m.base(volume);
    if(vb>quint64(image.size()) || quint64(v.size())>quint64(image.size())-vb ||
       image.mid(int(vb),v.size())!=v || base<vb+m.headerSize(volume) || base+size>vb+quint64(v.size()) ||
       image.mid(int(base),int(size))!=m.full(file) || m.headerSize(file)!=24 || m.tailSize(file))
        return "The parsed node does not map exactly to physical image bytes.";
    for(const auto& message: parser.getMessages())
        if(within(message.second,file) || message.second==volume)
            return "Parser diagnostics on this file/volume must be resolved before editing: " + message.first;
    return fileCheck(m.full(file), (dword(v,44)&EFI_FVB2_ERASE_POLARITY)!=0, quint32(base-vb));
}

bool FixedLayout::replace(const QByteArray& image, quint32 offset, const QByteArray& replacement,
                          bool bodyOnly, QByteArray& result, QString& error) {
    TreeModel original; FfsParser parser(&original);
    if(parser.parse(image)!=U_SUCCESS) { error="Original image cannot be parsed."; return false; }
    const QModelIndex file=original.findByBase(offset);
    error=eligibility(image,original,parser,file);
    if(!error.isEmpty()) return false;
    const QByteArray old=original.full(file);
    QByteArray fresh=bodyOnly ? old.left(24)+replacement : replacement;
    if(fresh.size()!=old.size()) { error="Replacement must have exactly the original size."; return false; }
    // Only the integrity bytes may differ in a supplied header.
    for(int i=0;i<24;++i) if(i!=16 && i!=17 && fresh[i]!=old[i]) {
        error="GUID, type, attributes, size and state must remain unchanged."; return false;
    }
    if(!bodyOnly) {
        const QByteArray v=original.header(file.parent());
        error=fileCheck(fresh,(dword(v,44)&EFI_FVB2_ERASE_POLARITY)!=0,original.base(file)-original.base(file.parent()));
        if(!error.isEmpty()) return false;
    }
    fresh[17]=(readByte(fresh,19)&FFS_ATTRIB_CHECKSUM) ? char(-sum8(fresh.mid(24))) : char(FFS_FIXED_CHECKSUM2);
    QByteArray h=fresh.left(24); h[16]=0; h[17]=0; h[23]=0;
    fresh[16]=char(-sum8(h));
    QByteArray candidate=image; candidate.replace(int(offset),old.size(),fresh);
    TreeModel checked; FfsParser verify(&checked);
    if(verify.parse(candidate)!=U_SUCCESS) { error="Candidate cannot be reparsed."; return false; }
    const QModelIndex target=checked.findByBase(offset);
    error=eligibility(candidate,checked,verify,target);
    if(!error.isEmpty()) return false;
    if(!sameLayout(original,QModelIndex(),checked,QModelIndex())) {
        error="Candidate changes parsed layout; fixed-layout edits cannot change structure."; return false;
    }
    result=candidate; error.clear(); return true;
}

bool FixedLayout::saveAs(const QString& source, const QString& destination,
                         const QByteArray& image, QString& error) {
    const QFileInfo src(source), dst(destination);
    if(destination.isEmpty() || destination.contains(QChar(0)) || dst.exists() || dst.isSymLink() ||
       src.absoluteFilePath()==dst.absoluteFilePath() ||
       (!src.canonicalFilePath().isEmpty() && src.canonicalFilePath()==dst.canonicalFilePath())) {
        error="Save As requires a new file; existing files and the original are never overwritten."; return false;
    }
    // Stage on the destination filesystem. Never use QSaveFile::commit(),
    // QFile::rename() or an existence check as the no-clobber guarantee.
    // Only this owned temporary pathname is eligible for automatic cleanup.
    const QString finalPath=dst.absoluteFilePath();
    StagedFileCleanup staged;
    {
        QTemporaryFile output(dst.absoluteDir().filePath(".uefitool-save-XXXXXX"));
        if(!output.open() || output.write(image)!=image.size() || !output.flush()) {
            error=output.errorString(); return false;
        }
        staged.path=output.fileName(); // Materialize unnamed Qt temporary files.
        if(staged.path.isEmpty()) { error="Could not name staged output."; return false; }
        output.setAutoRemove(false);
        // Destruction, not QTemporaryFile::close(), releases the native handle
        // on all Qt versions (Windows moves require that handle to be closed).
    }
#ifdef UEFITOOL_EDIT_TEST_HOOKS
    if(beforePublishForTest) beforePublishForTest(destination);
#endif
#ifdef Q_OS_UNIX
    // POSIX link atomically fails if ANY directory entry already occupies
    // finalPath, including a dangling symlink. QFile::link makes symlinks!
    if(::link(QFile::encodeName(staged.path).constData(), QFile::encodeName(finalPath).constData())!=0) {
        error=QString("Cannot publish new file without overwriting: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#elif defined(Q_OS_WIN)
    // No REPLACE_EXISTING or COPY_ALLOWED: same-filesystem, no-replace move.
    if(!MoveFileExW(reinterpret_cast<LPCWSTR>(staged.path.utf16()),
                    reinterpret_cast<LPCWSTR>(finalPath.utf16()), 0)) {
        error=QString("Cannot publish new file without overwriting (Windows error %1).").arg(GetLastError());
        return false;
    }
    // The old temporary name is no longer ours after a successful move.
    staged.path.clear();
#else
    error="Atomic no-replace saving is unsupported on this platform."; return false;
#endif
    // Verification errors must never remove or rewrite the destination: it
    // may have been changed by another process after successful publication.
    QFile check(finalPath);
    if(!check.open(QIODevice::ReadOnly) || check.readAll()!=image) { error="Saved output could not be verified."; return false; }
    error.clear(); return true;
}
