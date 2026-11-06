// Transactional recursive replacement over the NE tree. BSD-2-Clause.
#include "recursivereplace.h"
#include "LZMA/LzmaCompress.h"
#include "Tiano/EfiTianoCompress.h"
#include "Tiano/EfiTianoDecompress.h"
#include "ffs.h"
#include "utility.h"
#include <QMap>
#include <QtEndian>
#include <limits>

namespace
{
const int Limit = 128 * 1024 * 1024;
quint8 readByte(const QByteArray& b, int p)
{
    return quint8(b.at(p));
}
quint16 u16(const QByteArray& b, int p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData() + p));
}
quint32 u24(const QByteArray& b, int p)
{
    return readByte(b, p) | (readByte(b, p + 1) << 8) | (readByte(b, p + 2) << 16);
}
quint32 u32(const QByteArray& b, int p)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData() + p));
}
quint64 u64(const QByteArray& b, int p)
{
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData() + p));
}
void put16(QByteArray& b, int p, quint16 v)
{
    qToLittleEndian(v, reinterpret_cast<uchar*>(b.data() + p));
}
void put32(QByteArray& b, int p, quint32 v)
{
    qToLittleEndian(v, reinterpret_cast<uchar*>(b.data() + p));
}
void put64(QByteArray& b, int p, quint64 v)
{
    qToLittleEndian(v, reinterpret_cast<uchar*>(b.data() + p));
}
void put24(QByteArray& b, int p, quint32 v)
{
    for (int i = 0; i < 3; i++)
        b[p + i] = char(v >> (8 * i));
}
quint8 sum8(const QByteArray& b)
{
    quint8 s = 0;
    for (char c : b)
        s += quint8(c);
    return s;
}
int align(int n, int a)
{
    return (n + a - 1) & ~(a - 1);
}
bool contains(QModelIndex parent, QModelIndex n)
{
    for (; n.isValid(); n = n.parent())
        if (n == parent)
            return true;
    return false;
}
bool fail(QString& e, const QString& s)
{
    e = s;
    return false;
}
bool boundedAppend(QByteArray& b, const QByteArray& x, QString& e)
{
    if (x.size() > Limit - b.size())
        return fail(e, "Rebuilt buffer exceeds the 128 MiB editor limit.");
    b += x;
    return true;
}
int commonSize(const QByteArray& h)
{
    return h.size() >= 4 && u24(h, 0) == 0xffffff ? 8 : 4;
}
bool ffs3(const TreeModel& m, QModelIndex n)
{
    for (; n.isValid(); n = n.parent())
        if (m.type(n) == Types::Volume)
            return m.header(n).mid(16, 16) == EFI_FIRMWARE_FILE_SYSTEM3_GUID;
    return false;
}
bool pinned(const TreeModel& m, const QModelIndex& n)
{
    if (m.type(n) != Types::File)
        return true;
    const QByteArray h = m.header(n);
    const int t = m.subtype(n);
    return h.size() < 24 || (readByte(h, 19) & FFS_ATTRIB_FIXED) || h.left(16) == EFI_FFS_VOLUME_TOP_FILE_GUID
        || t == EFI_FV_FILETYPE_SECURITY_CORE || t == EFI_FV_FILETYPE_PEI_CORE || t == EFI_FV_FILETYPE_PEIM
        || t == EFI_FV_FILETYPE_COMBINED_PEIM_DRIVER;
}
bool relocationNeedsRebase(const TreeModel& m, const QModelIndex& n)
{
    if (m.type(n) == Types::File && pinned(m, n))
        return true;
    // A locally compressed stream is loaded elsewhere, not executed in place.
    if (m.type(n) == Types::Section && !m.uncompressedData(n).isEmpty())
        return false;
    for (int i = 0; i < m.rowCount(n); i++)
        if (relocationNeedsRebase(m, m.index(i, 0, n)))
            return true;
    return false;
}
// Only truly empty pad files can be regenerated/omitted. Opaque pad payloads
// are not free space, regardless of how the parser labels them.
bool filler(const TreeModel& m, const QModelIndex& n)
{
    if (m.type(n) == Types::FreeSpace)
        return true;
    return m.type(n) == Types::File && m.subtype(n) == EFI_FV_FILETYPE_PAD
        && (isUniformByte(m.body(n), 0xff) || isUniformByte(m.body(n), 0));
}
QVector<QModelIndex> significant(const TreeModel& m, const QModelIndex& p)
{
    QVector<QModelIndex> out;
    for (int i = 0; i < m.rowCount(p); i++)
    {
        auto n = m.index(i, 0, p);
        if (m.type(p) != Types::Volume || !filler(m, n))
            out += n;
    }
    return out;
}
QString volumeCheck(const TreeModel& m, const QModelIndex& n)
{
    const QByteArray h = m.header(n);
    if (h.size() < 72 || u32(h, 40) != EFI_FV_SIGNATURE || readByte(h, 55) != 2 || readByte(h, 54) != 0)
        return "Only standard revision-2 firmware volumes can be rebuilt.";
    if (h.mid(16, 16) != EFI_FIRMWARE_FILE_SYSTEM2_GUID && h.mid(16, 16) != EFI_FIRMWARE_FILE_SYSTEM3_GUID)
        return "Unsupported firmware-volume filesystem (only FFS2/FFS3).";
    const int len = u16(h, 48), ext = u16(h, 52);
    if (len < 72 || len > h.size() || (len & 7) || u64(h, 32) != m.fullSize(n) || m.fullSize(n) > Limit)
        return "Invalid firmware-volume header length or extent.";
    quint16 sum = 0;
    for (int p = 0; p < len; p += 2)
        sum += u16(h, p);
    if (sum)
        return "Invalid firmware-volume header checksum.";
    if (ext && (ext < len || ext > h.size() - 20 || u32(h, ext + 16) != 20))
        return "FV extended entries (including UsedSize/authentication) are not supported; only name-only "
               "extensions are rebuilt.";
    quint64 total = 0;
    bool end = false;
    for (int p = 56; p + 8 <= len; p += 8)
    {
        quint32 count = u32(h, p), size = u32(h, p + 4);
        if (!count && !size)
        {
            end = true;
            break;
        }
        if (!count || !size)
            return "Malformed FV block map.";
        total += quint64(count) * size;
        if (total > m.fullSize(n))
            return "FV block map exceeds its physical extent.";
    }
    if (!end || total != m.fullSize(n))
        return "FV block map does not cover the volume.";
    if (m.parsingData(n).size() >= int(sizeof(VOLUME_PARSING_DATA)))
    {
        VOLUME_PARSING_DATA p;
        memcpy(&p, m.parsingData(n).constData(), sizeof(p));
        if (p.hasAppleCrc32 || p.hasValidUsedSpace || p.requiresSectionAlignmentQuirk)
            return "Apple CRC/UsedSpace or nonstandard section alignment needs a dedicated builder.";
    }
    return {};
}
QString fileCheck(const TreeModel& m, const QModelIndex& n)
{
    const QByteArray h = m.header(n), b = m.body(n);
    if (h.size() != 24 && h.size() != 32)
        return "Nonstandard FFS header size.";
    if (m.tailSize(n) || m.type(n.parent()) != Types::Volume)
        return "FFS tails/non-volume parents are unsupported.";
    const int a = readByte(h, 19);
    if (m.subtype(n) < EFI_FV_FILETYPE_RAW || m.subtype(n) > EFI_FV_FILETYPE_MM_CORE_STANDALONE)
        return "Unsupported FFS file type.";
    if (a & 0x80)
        return "Reserved FFS attribute is set.";
    const unsigned power = (a & FFS_ATTRIB_DATA_ALIGNMENT2)
        ? ffsAlignment2Table[(a & FFS_ATTRIB_DATA_ALIGNMENT) >> 3]
        : ffsAlignmentTable[(a & FFS_ATTRIB_DATA_ALIGNMENT) >> 3];
    if ((m.offset(n) & 7) || ((quint64(m.offset(n)) + h.size()) % (quint64(1) << power)))
        return "Original FFS header/data alignment is invalid.";
    if ((a & FFS_ATTRIB_LARGE_FILE)
            ? (!ffs3(m, n) || h.size() != 32 || u24(h, 20) != 0xffffff || u64(h, 24) != m.fullSize(n))
            : (h.size() != 24 || u24(h, 20) != m.fullSize(n)))
        return "Invalid FFS2/FFS3 size encoding (large files require FFS3).";
    const QByteArray vh = m.header(n.parent());
    if (vh.size() < 72)
        return "Truncated parent FV.";
    if (readByte(h, 23) != ((u32(vh, 44) & EFI_FVB2_ERASE_POLARITY) ? 0xf8 : 7))
        return "Only canonical DATA_VALID FFS files are editable.";
    QByteArray c = h;
    c[17] = 0;
    c[23] = 0;
    if (sum8(c))
        return "Invalid original FFS header checksum.";
    if (a & FFS_ATTRIB_CHECKSUM)
    {
        if (quint8(sum8(b) + readByte(h, 17)))
            return "Invalid original FFS data checksum.";
    }
    else if (readByte(h, 17) != FFS_FIXED_CHECKSUM2)
        return "Invalid fixed FFS checksum.";
    return {};
}
// Identify the local codec from parsing metadata/header, never the inherited
// compressed flag (all descendants of a compressed section carry that flag).
bool codec(const TreeModel& m, const QModelIndex& n, int& algorithm, quint32& dictionary, QString& e)
{
    algorithm = COMPRESSION_ALGORITHM_NONE;
    dictionary = 0;
    QByteArray h = m.header(n);
    const int c = commonSize(h);
    if (m.subtype(n) == EFI_SECTION_COMPRESSION)
    {
        if (h.size() != c + 5 || m.parsingData(n).size() != sizeof(COMPRESSED_SECTION_PARSING_DATA))
            return fail(e, "Missing compression metadata.");
        COMPRESSED_SECTION_PARSING_DATA p;
        memcpy(&p, m.parsingData(n).constData(), sizeof(p));
        algorithm = p.algorithm;
        dictionary = p.dictionarySize;
    }
    else if (m.subtype(n) == EFI_SECTION_GUID_DEFINED)
    {
        if (h.size() < c + 20 || u16(h, c + 16) != h.size())
            return fail(e, "Invalid guided-section data offset.");
        const QByteArray guid = h.mid(c, 16);
        if (guid == EFI_GUIDED_SECTION_LZMA || guid == EFI_GUIDED_SECTION_LZMA_HP
            || guid == EFI_GUIDED_SECTION_LZMA_MS)
            algorithm = COMPRESSION_ALGORITHM_LZMA;
        else if (guid == EFI_GUIDED_SECTION_TIANO)
        {
            UINT8 a = 0;
            UINT32 d = 0;
            QByteArray out, other;
            if (decompress(m.body(n), EFI_STANDARD_COMPRESSION, a, d, out, other) != U_SUCCESS)
                return fail(e, "Cannot decode original Tiano guided section.");
            // Primary is Tiano only when both decoders succeed; when just
            // EFI succeeds, decompress() returns EFI bytes in primary instead.
            if ((a == COMPRESSION_ALGORITHM_EFI11 || a == COMPRESSION_ALGORITHM_TIANO)
                && out == m.uncompressedData(n))
                algorithm = a;
            else if (a == COMPRESSION_ALGORITHM_UNDECIDED && out == m.uncompressedData(n))
                algorithm = COMPRESSION_ALGORITHM_TIANO;
            else if (a == COMPRESSION_ALGORITHM_UNDECIDED && other == m.uncompressedData(n))
                algorithm = COMPRESSION_ALGORITHM_EFI11;
            else
                return fail(e, "Ambiguous guided Tiano/EFI compression.");
        }
        else if (guid == EFI_GUIDED_SECTION_CRC32)
        {
            if (h.size() != c + 24)
                return fail(e, "Nonstandard CRC32 guided header.");
        }
        else
            return fail(e,
                "Unsupported guided-section codec/processing GUID: " + m.name(n)
                    + ". No legacy-parser fallback.");
        if (m.parsingData(n).size() >= int(sizeof(GUIDED_SECTION_PARSING_DATA)))
        {
            GUIDED_SECTION_PARSING_DATA p;
            memcpy(&p, m.parsingData(n).constData(), sizeof(p));
            dictionary = p.dictionarySize;
        }
    }
    if (algorithm != COMPRESSION_ALGORITHM_NONE && algorithm != COMPRESSION_ALGORITHM_LZMA
        && algorithm != COMPRESSION_ALGORITHM_TIANO && algorithm != COMPRESSION_ALGORITHM_EFI11)
        return fail(e, "Unsupported compression algorithm (only none, EFI 1.1, Tiano and LZMA are rebuilt).");
    if (algorithm == COMPRESSION_ALGORITHM_LZMA && (dictionary < 4096 || dictionary > quint32(Limit)))
        return fail(e, "LZMA dictionary is outside the supported 4 KiB–128 MiB range.");
    return true;
}
bool encode(const QByteArray& data, int algorithm, quint32 dictionary, QByteArray& out, QString& e)
{
    if (data.size() > Limit)
        return fail(e, "Uncompressed data exceeds editor limit.");
    if (algorithm == COMPRESSION_ALGORITHM_NONE)
    {
        out = data;
        return true;
    }
    UINT32 size = 0;
    USTATUS status;
    if (algorithm == COMPRESSION_ALGORITHM_LZMA)
        status = LzmaCompress(
            reinterpret_cast<const UINT8*>(data.constData()), data.size(), nullptr, &size, dictionary);
    else
        status = (algorithm == COMPRESSION_ALGORITHM_EFI11 ? EfiCompress : TianoCompress)(
            data.constData(), data.size(), nullptr, &size);
    if (status != U_BUFFER_TOO_SMALL || size > quint32(Limit))
        return fail(e, "Compression size query failed or exceeds limit.");
    out.resize(int(size));
    if (algorithm == COMPRESSION_ALGORITHM_LZMA)
        status = LzmaCompress(reinterpret_cast<const UINT8*>(data.constData()), data.size(),
            reinterpret_cast<UINT8*>(out.data()), &size, dictionary);
    else
        status = (algorithm == COMPRESSION_ALGORITHM_EFI11 ? EfiCompress : TianoCompress)(
            data.constData(), data.size(), out.data(), &size);
    if (status != U_SUCCESS)
        return fail(e, "Compression failed.");
    out.resize(int(size));
    QByteArray decoded;
    if (algorithm == COMPRESSION_ALGORITHM_EFI11 || algorithm == COMPRESSION_ALGORITHM_TIANO)
    {
        // Verify with the selected decoder, not whichever NE decoder happens
        // to accept the result. The other codec can succeed with wrong bytes.
        UINT32 decodedSize = 0, scratchSize = 0;
        if (EfiTianoGetInfo(out.constData(), out.size(), &decodedSize, &scratchSize) != U_SUCCESS
            || decodedSize != quint32(data.size()) || scratchSize > quint32(Limit))
            return fail(e, "Recompression verification size query failed.");
        decoded.resize(int(decodedSize));
        QByteArray scratch(int(scratchSize), 0);
        if ((algorithm == COMPRESSION_ALGORITHM_EFI11 ? EfiDecompress : TianoDecompress)(
                out.constData(), out.size(), decoded.data(), decodedSize, scratch.data(), scratchSize)
            != U_SUCCESS)
            return fail(e, "Recompression failed selected-codec decompression verification.");
    }
    else
    {
        UINT8 a = 0;
        UINT32 d = 0;
        QByteArray alternate;
        if (decompress(out, EFI_CUSTOMIZED_COMPRESSION, a, d, decoded, alternate) != U_SUCCESS
            || a != algorithm || d != dictionary)
            return fail(e, "Recompression failed selected-codec decompression verification.");
    }
    if (decoded != data)
        return fail(e, "Recompression failed byte-for-byte decompression verification.");
    return true;
}
void fileChecksum(QByteArray& h, const QByteArray& b)
{
    h[16] = 0;
    h[17] = 0;
    const char state = h.at(23);
    h[23] = 0;
    h[16] = char(-sum8(h));
    h[23] = state;
    h[17] = (readByte(h, 19) & FFS_ATTRIB_CHECKSUM) ? char(-sum8(b)) : char(FFS_FIXED_CHECKSUM2);
}
bool finishFile(QByteArray h, const QByteArray& b, bool version3, QByteArray& out, QString& e)
{
    if (h.size() != 24 && h.size() != 32)
        return fail(e, "Unsupported replacement FFS header.");
    if (h.size() + b.size() >= 0xffffff && h.size() == 24)
    {
        if (!version3)
            return fail(e, "FFS2 file exceeds the 24-bit size limit; use an FFS3 volume.");
        h += QByteArray(8, 0);
        h[19] = char(readByte(h, 19) | FFS_ATTRIB_LARGE_FILE);
    }
    if (h.size() == 32)
    {
        if (!version3)
            return fail(e, "Large FFS files require FFS3.");
        put24(h, 20, 0xffffff);
        put64(h, 24, quint64(h.size()) + b.size());
    }
    else
        put24(h, 20, h.size() + b.size());
    fileChecksum(h, b);
    out = h;
    return boundedAppend(out, b, e);
}
bool finishSection(QByteArray h, const QByteArray& b, bool version3, QByteArray& out, QString& e)
{
    int c = commonSize(h);
    if (h.size() < c)
        return fail(e, "Truncated replacement section header.");
    if (c == 8 && !version3)
        return fail(e, "Extended section headers require an FFS3 containing volume.");
    if (h.size() + b.size() >= 0xffffff && c == 4)
    {
        if (!version3)
            return fail(e, "Section exceeds 24-bit size limit in FFS2.");
        h.insert(4, QByteArray(4, 0));
        c = 8;
        if (readByte(h, 3) == EFI_SECTION_GUID_DEFINED)
            put16(h, 24, h.size());
    }
    if (c == 8)
    {
        put24(h, 0, 0xffffff);
        put32(h, 4, h.size() + b.size());
    }
    else
        put24(h, 0, h.size() + b.size());
    out = h;
    return boundedAppend(out, b, e);
}
QByteArray padFile(int size, char empty)
{
    QByteArray h(24, 0), b(size - 24, empty);
    h.replace(0, 16, EFI_FFS_PAD_FILE_GUID);
    h[18] = char(EFI_FV_FILETYPE_PAD);
    h[23] = empty == char(0xff) ? char(0xf8) : char(7);
    put24(h, 20, size);
    fileChecksum(h, b);
    return h + b;
}
struct Builder
{
    const TreeModel& m;
    QModelIndex target;
    QByteArray replacement;
    bool bodyOnly;
    QString& error;
    QMap<RecursiveReplace::Path, QByteArray> rebuilt;
    bool build(const QModelIndex& n, QByteArray& out)
    {
        if (!contains(n, target))
        {
            out = m.full(n);
            return true;
        }
        if (n == target)
        {
            if (!replaceTarget(n, out))
                return false;
        }
        else if (m.type(n) == Types::Volume)
        {
            if (!volume(n, out))
                return false;
        }
        else
        {
            QByteArray body;
            if (!stream(n, body))
                return false;
            if (m.type(n) == Types::File)
            {
                if (!finishFile(m.header(n), body, ffs3(m, n), out, error))
                    return false;
            }
            else if (m.type(n) == Types::Section)
            {
                QByteArray h = m.header(n), encoded;
                int a;
                quint32 d;
                if (!codec(m, n, a, d, error) || !encode(body, a, d, encoded, error))
                    return false;
                if (m.subtype(n) == EFI_SECTION_COMPRESSION)
                    put32(h, commonSize(h), body.size());
                if (m.subtype(n) == EFI_SECTION_GUID_DEFINED
                    && h.mid(commonSize(h), 16) == EFI_GUIDED_SECTION_CRC32)
                    put32(h, commonSize(h) + 20,
                        crc32(0, reinterpret_cast<const Bytef*>(encoded.constData()), encoded.size()));
                if (!finishSection(h, encoded, ffs3(m, n), out, error))
                    return false;
            }
            else
                return fail(error, "Unsupported rebuild ancestor: " + m.name(n));
        }
        rebuilt.insert(RecursiveReplace::path(n), out);
        return true;
    }
    bool replaceTarget(const QModelIndex& n, QByteArray& out)
    {
        QByteArray h = m.header(n), b;
        if (bodyOnly)
            b = replacement;
        else
        {
            const bool file = m.type(n) == Types::File;
            const int min = file ? 24 : 4;
            if (replacement.size() < min)
                return fail(error, "Replacement header is truncated.");
            int hs = file ? ((readByte(replacement, 19) & FFS_ATTRIB_LARGE_FILE) ? 32 : 24)
                          : commonSize(replacement);
            if (replacement.size() < hs)
                return fail(error, "Extended replacement header is truncated.");
            quint64 size = file ? (hs == 32 ? u64(replacement, 24) : u24(replacement, 20))
                                : (hs == 8 ? u32(replacement, 4) : u24(replacement, 0));
            if (size != quint64(replacement.size()))
                return fail(error, "As-is replacement size field does not match the supplied bytes.");
            if (file)
            {
                if (readByte(replacement, 18) != m.subtype(n)
                    || (readByte(replacement, 19) & ~FFS_ATTRIB_LARGE_FILE)
                        != (readByte(h, 19) & ~FFS_ATTRIB_LARGE_FILE))
                    return fail(
                        error, "As-is FFS replacement must preserve file type and non-size attributes.");
                // A file extracted from a volume with opposite erase polarity is
                // normalized to the destination's validated live state.
                h = replacement.left(hs);
                if (h.left(16) == EFI_FFS_VOLUME_TOP_FILE_GUID)
                    return fail(error, "Creating a VTF requires address/entry-point fixups, which are not implemented.");
                h[23] = m.header(n).at(23);
            }
            else
            {
                if (readByte(replacement, 3) != m.subtype(n))
                    return fail(error, "As-is section replacement must preserve section type.");
                // Keep subtype-specific header length, allowing common-header promotion.
                hs += m.headerSize(n) - commonSize(m.header(n));
                if (replacement.size() < hs)
                    return fail(error, "Subtype-specific section header is truncated.");
                h = replacement.left(hs);
                if (m.subtype(n) == EFI_SECTION_GUID_DEFINED
                    && (h.mid(commonSize(h), 16) != m.header(n).mid(commonSize(m.header(n)), 16)
                        || u16(h, commonSize(h) + 16) != hs))
                    return fail(
                        error, "As-is guided section must preserve processing GUID and valid data offset.");
            }
            b = replacement.mid(hs);
        }
        if (m.type(n) == Types::File)
            return finishFile(h, b, ffs3(m, n), out, error);
        // Match old_engine Replace body: decode-aware section bodies. NE keeps
        // separate Extract body (serialized) and Extract uncompressed actions.
        if (bodyOnly)
        {
            int a;
            quint32 d;
            QByteArray encoded;
            if (!codec(m, n, a, d, error) || !encode(b, a, d, encoded, error))
                return false;
            if (m.subtype(n) == EFI_SECTION_COMPRESSION)
                put32(h, commonSize(h), b.size());
            b = encoded;
        }
        if (m.subtype(n) == EFI_SECTION_GUID_DEFINED && h.mid(commonSize(h), 16) == EFI_GUIDED_SECTION_CRC32)
            put32(h, commonSize(h) + 20, crc32(0, reinterpret_cast<const Bytef*>(b.constData()), b.size()));
        return finishSection(h, b, ffs3(m, n), out, error);
    }
    bool stream(const QModelIndex& n, QByteArray& out)
    {
        int a = COMPRESSION_ALGORITHM_NONE;
        quint32 d = 0;
        if (m.type(n) == Types::Section && !codec(m, n, a, d, error))
            return false;
        const QByteArray original = a == COMPRESSION_ALGORITHM_NONE ? m.body(n) : m.uncompressedData(n);
        out.clear();
        int previous = 0;
        for (int i = 0; i < m.rowCount(n); i++)
        {
            auto child = m.index(i, 0, n);
            const int start = int(m.offset(child)) - int(m.headerSize(n));
            if (start < previous || start > original.size()
                || m.fullSize(child) > quint32(original.size() - start)
                || original.mid(start, m.fullSize(child)) != m.full(child))
                return fail(
                    error, "Child does not map exactly into its parent's decoded body: " + m.name(child));
            QByteArray gap = original.mid(previous, start - previous);
            if (out.size() == previous)
                out += gap;
            else
            {
                if (gap.size() > 3 || (!isUniformByte(gap, 0) && !isUniformByte(gap, 0xff)))
                    return fail(error, "Cannot relocate opaque bytes between sections.");
                if (m.type(child) != Types::Section)
                    return fail(error,
                        "Relocating nested volumes or opaque nodes in a section stream is unsupported.");
                out += QByteArray(align(out.size(), 4) - out.size(), 0);
            }
            QByteArray fresh;
            if (!build(child, fresh) || !boundedAppend(out, fresh, error))
                return false;
            previous = start + m.fullSize(child);
        }
        return boundedAppend(out, original.mid(previous), error);
    }
    bool volume(const QModelIndex& n, QByteArray& out)
    {
        const QByteArray original = m.full(n);
        out = m.header(n);
        const char empty = (u32(out, 44) & EFI_FVB2_ERASE_POLARITY) ? char(0xff) : char(0);
        int oldEnd = m.headerSize(n);
        for (int i = 0; i < m.rowCount(n); i++)
        {
            auto child = m.index(i, 0, n);
            const int start = m.offset(child);
            if (start < oldEnd || start > original.size()
                || m.fullSize(child) > quint32(original.size() - start)
                || original.mid(start, m.fullSize(child)) != m.full(child))
                return fail(error, "FV child extent is inconsistent.");
            const QByteArray gap = original.mid(oldEnd, start - oldEnd);
            if (!isUniformByte(gap, quint8(empty)))
                return fail(error, "Opaque inter-file bytes cannot be rebuilt.");
            oldEnd = start + m.fullSize(child);
            if (m.type(child) == Types::FreeSpace)
            {
                if (!isUniformByte(m.full(child), quint8(empty)))
                    return fail(error, "Nonempty free-space node.");
                continue;
            }
            QByteArray fresh;
            if (!build(child, fresh))
                return false;
            if (m.type(child) != Types::File)
            {
                if (out.size() > start)
                    return fail(error, "Growing file overlaps pinned opaque FV data.");
                const int gapSize = start - out.size();
                if (gapSize)
                    out += QByteArray(gapSize, empty);
            }
            else
            {
                const int next = align(out.size(), 8);
                out += QByteArray(next - out.size(), empty);
                const bool fixed = relocationNeedsRebase(m, child)
                    || (m.subtype(child) == EFI_FV_FILETYPE_PAD && !filler(m, child))
                    || (m.subtype(child) > EFI_FV_FILETYPE_MM_CORE_STANDALONE
                        && m.subtype(child) != EFI_FV_FILETYPE_PAD);
                if (fixed && out.size() != start)
                    return fail(error,
                        "Relocation would move a fixed-address/SEC/PEI/VTF or opaque file: " + m.name(child)
                            + ". Rebasing is not implemented.");
                const int attr = readByte(fresh, 19), hs = (attr & FFS_ATTRIB_LARGE_FILE) ? 32 : 24;
                const int exponent = (attr & FFS_ATTRIB_DATA_ALIGNMENT2)
                    ? ffsAlignment2Table[(attr & FFS_ATTRIB_DATA_ALIGNMENT) >> 3]
                    : ffsAlignmentTable[(attr & FFS_ATTRIB_DATA_ALIGNMENT) >> 3];
                if (exponent > 24)
                    return fail(error, "Excessive file alignment.");
                const int alignment = 1 << exponent;
                if ((out.size() + hs) % alignment)
                {
                    int pad = alignment - (out.size() + hs) % alignment;
                    while (pad < 24)
                        pad += alignment;
                    if (fixed)
                        return fail(error, "Fixed-address/opaque file requires new alignment padding.");
                    if (pad >= 0xffffff || pad > original.size() - out.size())
                        return fail(error, "FFS alignment padding overflows the volume.");
                    out += padFile(pad, empty);
                }
            }
            if (fresh.size() > original.size() - out.size())
                return fail(error,
                    "Firmware volume overflow in " + m.name(n)
                        + "; neither physical nor nested FV extents are grown.");
            out += fresh;
        }
        if (!isUniformByte(original.mid(oldEnd), quint8(empty)))
            return fail(error, "Unmapped nonempty FV trailer.");
        if (out.size() > original.size())
            return fail(error, "Firmware volume overflow.");
        out += QByteArray(original.size() - out.size(), empty);
        // FV extent, block map and name-only extension are unchanged; preserve
        // the validated header (including its checksum) exactly.
        return true;
    }
    bool verify(const QModelIndex& old, const TreeModel& check, const QModelIndex& fresh,
        RecursiveReplace::Path& selected)
    {
        const auto key = RecursiveReplace::path(old);
        if (!contains(old, target))
            return m.full(old) == check.full(fresh)
                || fail(error, "Untouched sibling changed during reparse.");
        if (!rebuilt.contains(key) || rebuilt.value(key) != check.full(fresh))
            return fail(error, "Reparsed rebuilt node differs from the serialized candidate.");
        if (old == target)
        {
            selected = RecursiveReplace::path(fresh);
            return true;
        }
        auto a = significant(m, old), b = significant(check, fresh);
        if (a.size() != b.size())
            return fail(error, "Reparse lost or added a non-padding sibling.");
        for (int i = 0; i < a.size(); i++)
            if (!verify(a[i], check, b[i], selected))
                return false;
        return true;
    }
};
}

RecursiveReplace::Path RecursiveReplace::path(const QModelIndex& node)
{
    Path p;
    for (auto n = node; n.isValid(); n = n.parent())
        p.prepend(n.row());
    return p;
}
QModelIndex RecursiveReplace::index(const TreeModel& m, const Path& p)
{
    QModelIndex n;
    for (int r : p)
    {
        if (r < 0 || r >= m.rowCount(n))
            return {};
        n = m.index(r, 0, n);
    }
    return n;
}
QString RecursiveReplace::eligibility(const QByteArray& image, const TreeModel& m, const QModelIndex& n)
{
    if (!n.isValid() || (m.type(n) != Types::File && m.type(n) != Types::Section))
        return "Select an FFS file or section.";
    if (m.type(n) == Types::File && m.subtype(n) == EFI_FV_FILETYPE_PAD)
        return "Padding files are not replacement targets.";
    if (relocationNeedsRebase(m, n))
        return "Selected subtree contains fixed-address/SEC/PEI/VTF contents; executable rebasing is not implemented.";
    QModelIndex outer;
    for (auto p = n; p.isValid(); p = p.parent())
    {
        QString reason;
        if (m.type(p) == Types::File)
        {
            reason = fileCheck(m, p);
            if (reason.isEmpty() && pinned(m, p))
                reason = "Fixed-address/SEC/PEI/VTF replacement requires executable rebasing, which is not "
                         "implemented.";
        }
        else if (m.type(p) == Types::Volume)
        {
            reason = volumeCheck(m, p);
            outer = p;
        }
        else if (m.type(p) == Types::Section)
        {
            int a;
            quint32 d;
            if (!codec(m, p, a, d, reason))
            {
            }
            if (p != n && m.subtype(p) != EFI_SECTION_COMPRESSION && m.subtype(p) != EFI_SECTION_GUID_DEFINED
                && m.subtype(p) != EFI_SECTION_DISPOSABLE
                && m.subtype(p) != EFI_SECTION_FIRMWARE_VOLUME_IMAGE)
                reason = "Unsupported section container: " + m.name(p);
        }
        else if (m.type(p) != Types::Image && m.type(p) != Types::Region)
            reason = "Unsupported ancestor (capsules and vendor containers need dedicated builders).";
        if (!reason.isEmpty())
            return m.name(p) + ": " + reason;
    }
    if (!outer.isValid())
        return "No rebuildable firmware volume ancestor.";
    const quint64 base = m.base(outer), size = m.fullSize(outer);
    if (base > quint64(image.size()) || size > quint64(image.size()) - base
        || image.mid(int(base), int(size)) != m.full(outer))
        return "Outermost FV does not map exactly to physical image bytes.";
    return {};
}
QStringList RecursiveReplace::warnings(const FfsParser& p)
{
    QStringList w;
    if (p.hasProtectedRanges() || !p.getSecurityInfo().isEmpty())
        w << "Recognized protection/security metadata is present. Replacement can invalidate hashes or "
             "signatures; authentication is not regenerated.";
    w << "Embedded executable signatures and platform dependencies may be invalidated. Structural reparse is "
         "not a boot or authenticity guarantee.";
    return w;
}
QByteArray RecursiveReplace::replacementBody(const TreeModel& m, const QModelIndex& n)
{
    if (m.type(n) == Types::Section
        && (m.subtype(n) == EFI_SECTION_COMPRESSION || m.subtype(n) == EFI_SECTION_GUID_DEFINED))
    {
        int a;
        quint32 d;
        QString e;
        if (codec(m, n, a, d, e) && a != COMPRESSION_ALGORITHM_NONE)
            return m.uncompressedData(n);
    }
    return m.body(n);
}
bool RecursiveReplace::replace(const QByteArray& image, const Path& targetPath, const QByteArray& replacement,
    bool bodyOnly, QByteArray& result, QString& error, Path* resultPath)
{
    error.clear();
    if (image.size() > Limit || replacement.size() > Limit)
        return fail(error, "Image/replacement exceeds the 128 MiB editor limit.");
    TreeModel m;
    FfsParser p(&m);
    if (p.parse(image) != U_SUCCESS)
        return fail(error, "Original image could not be parsed.");
    auto n = index(m, targetPath);
    error = eligibility(image, m, n);
    if (!error.isEmpty())
        return false;
    if (replacement == (bodyOnly ? replacementBody(m, n) : m.full(n)))
    {
        result = image;
        if (resultPath)
            *resultPath = targetPath;
        return true;
    }
    QModelIndex outer;
    for (auto a = n; a.isValid(); a = a.parent())
        if (m.type(a) == Types::Volume)
            outer = a;
    Builder b { m, n, replacement, bodyOnly, error, {} };
    QByteArray rebuilt;
    if (!b.build(outer, rebuilt))
        return false;
    if (rebuilt.size() != int(m.fullSize(outer)))
        return fail(error, "Physical FV extent changed.");
    QByteArray candidate = image;
    candidate.replace(m.base(outer), rebuilt.size(), rebuilt);
    TreeModel checked;
    FfsParser verify(&checked);
    if (verify.parse(candidate) != U_SUCCESS)
        return fail(error, "Candidate reparse failed.");
    // Existing warnings are not a blanket prohibition. This exact NE hash
    // diagnostic is expected after editing protected bytes. The GUI warns
    // before applying and displays it after. Structural checksums are not waived.
    QMap<QString, int> messages;
    for (const auto& msg : p.getMessages())
        messages[msg.first]++;
    for (const auto& msg : verify.getMessages())
    {
        const bool invalidatedHash = msg.first.startsWith("checkProtectedRanges: ")
            && msg.first.contains("protected range")
            && msg.first.endsWith("hash mismatch, opened image may refuse to boot");
        if (--messages[msg.first] < 0 && !invalidatedHash)
            return fail(error, "Candidate introduces a parser diagnostic: " + msg.first);
    }
    Path selected;
    if (!b.verify(outer, checked, index(checked, path(outer)), selected))
        return false;
    result = candidate;
    if (resultPath)
        *resultPath = selected;
    return true;
}
