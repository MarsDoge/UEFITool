// Synthetic standard FV/FFS fixtures, NOT vendor firmware.
#include "../common/fixedlayout.h"
#include "../common/ffs.h"
#include "../UEFITool/uefitool.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#include <QAction>
#include <QTimer>
#include <QPushButton>
#include <QtEndian>
#include <cstdio>
#include <cstdlib>
static int checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void put16(QByteArray& a,int p,quint16 v) { qToLittleEndian(v,reinterpret_cast<uchar*>(a.data()+p)); }
static void put32(QByteArray& a,int p,quint32 v) { qToLittleEndian(v,reinterpret_cast<uchar*>(a.data()+p)); }
static void put64(QByteArray& a,int p,quint64 v) { qToLittleEndian(v,reinterpret_cast<uchar*>(a.data()+p)); }
static quint8 sum8(const QByteArray& a) { quint8 s=0; for(char c:a) s+=quint8(c); return s; }
static void checksums(QByteArray& a) {
    a[72+17]=(quint8(a[72+19])&0x40) ? char(-sum8(a.mid(96,32))) : char(0xaa);
    QByteArray h=a.mid(72,24); h[16]=0; h[17]=0; h[23]=0; a[88]=char(-sum8(h));
    put16(a,50,0); quint16 sum=0; for(int i=0;i<72;i+=2) sum+=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(a.constData()+i)); put16(a,50,quint16(-sum));
}
static QByteArray fixture(bool checksum=true, bool polarity=true) {
    // NE currently diagnoses zero-filled free space as an invalid file size.
    // The zero-polarity fixture therefore fills its volume exactly.
    const int volumeSize = polarity ? 4096 : 128;
    QByteArray a(volumeSize, polarity ? char(0xff) : char(0));
    a.replace(0,72,QByteArray(72,0)); a.replace(16,16,EFI_FIRMWARE_FILE_SYSTEM2_GUID);
    put64(a,32,volumeSize); put32(a,40,EFI_FV_SIGNATURE); put32(a,44,polarity ? EFI_FVB2_ERASE_POLARITY : 0);
    put16(a,48,72); a[55]=2; put32(a,56,1); put32(a,60,volumeSize);
    a.replace(72,24,QByteArray(24,0)); a.replace(72,16,QByteArray::fromHex("102030405060708090a0b0c0d0e0f001"));
    a[90]=EFI_FV_FILETYPE_RAW; a[91]=checksum ? 0x40 : 0; a[92]=56; a[95]=polarity ? char(0xf8) : 7;
    a.replace(96,32,QByteArray(32,'A')); checksums(a); return a;
}
static void write(const QString& p,const QByteArray& a) { QFile f(p); CHECK(f.open(QIODevice::WriteOnly)); CHECK(f.write(a)==a.size()); }
static QByteArray read(const QString& p) { QFile f(p); CHECK(f.open(QIODevice::ReadOnly)); return f.readAll(); }
static void answer(QMessageBox::StandardButton button) {
    QTimer::singleShot(0,[button]() { for(QWidget* w:QApplication::topLevelWidgets()) if(auto box=qobject_cast<QMessageBox*>(w)) { box->button(button)->click(); return; } });
}
static void menuFileAction(QAction* action, const QString& path) {
    QTimer driver;
    int ticks = 0;
    QObject::connect(&driver, &QTimer::timeout, [&]() {
        CHECK(++ticks < 200);
        for(QWidget* w:QApplication::topLevelWidgets()) {
            if(!w->isVisible()) continue;
            if(auto dialog=qobject_cast<QFileDialog*>(w)) {
                if(ticks==1) fprintf(stderr,"Driving file dialog: %s -> %s\n",qPrintable(dialog->windowTitle()),qPrintable(path));
                dialog->findChild<QLineEdit*>("fileNameEdit")->setText(path);
                QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);
                return;
            }
            if(auto box=qobject_cast<QMessageBox*>(w)) {
                fprintf(stderr,"Driving message: %s\n",qPrintable(box->text()));
                CHECK(box->standardButtons() & QMessageBox::Yes);
                box->button(QMessageBox::Yes)->click(); return;
            }
        }
    });
    driver.start(20); action->trigger(); driver.stop();
}
int main(int argc,char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv); app.setOrganizationName("UEFIToolSyntheticTests"); app.setApplicationName("FixedLayoutTests");
    QTemporaryDir dir; CHECK(dir.isValid());
    QString error; QByteArray result;
    for(bool checksum:{false,true}) for(bool polarity:{false,true}) {
        QByteArray a=fixture(checksum,polarity);
        bool ok=FixedLayout::replace(a,72,QByteArray(32,'B'),true,result,error);
        if(!ok) fprintf(stderr,"eligibility: %s\n",qPrintable(error)); CHECK(ok);
        CHECK(result.size()==a.size()); CHECK(result.left(72)==a.left(72)); CHECK(result.mid(128)==a.mid(128));
        CHECK(result.mid(96,32)==QByteArray(32,'B'));
        CHECK(FixedLayout::replace(a,72,result.mid(72,56),false,result,error));
        QByteArray sentinel("untouched");
        CHECK(!FixedLayout::replace(a,72,QByteArray(31,'C'),true,sentinel,error)); CHECK(sentinel=="untouched");
        QByteArray bad=result.mid(72,56); bad[0]=bad.at(0)^1;
        CHECK(!FixedLayout::replace(a,72,bad,false,sentinel,error));
        for(int field=0;field<24;++field) {
            if(field==16 || field==17) continue;
            bad=result.mid(72,56); bad[field]=bad.at(field)^1;
            CHECK(!FixedLayout::replace(a,72,bad,false,sentinel,error));
            CHECK(sentinel=="untouched");
        }
        bad=result.mid(72,56); bad[17]=bad.at(17)^1;
        CHECK(!FixedLayout::replace(a,72,bad,false,sentinel,error));
        for(int pos:{88,89,95,50,52,55,60}) {
            bad=a; bad[pos]=bad.at(pos)^1;
            CHECK(!FixedLayout::replace(bad,72,QByteArray(32,'B'),true,sentinel,error));
        }
        bad=a; bad[91]=char(0xc0); checksums(bad);
        CHECK(!FixedLayout::replace(bad,72,QByteArray(32,'B'),true,sentinel,error));
        CHECK(!FixedLayout::replace(a,73,QByteArray(32,'B'),true,sentinel,error));
    }
    QByteArray a=fixture();
    // Capsule ancestry, including a standard unsigned capsule, is read-only.
    QByteArray capsule(32,0); capsule.replace(0,16,EFI_CAPSULE_GUID);
    put32(capsule,16,32); put32(capsule,24,32+a.size()); capsule+=a;
    CHECK(!FixedLayout::replace(capsule,104,QByteArray(32,'B'),true,result,error));
    // Name-only standard FV extension: same-size editing preserves it verbatim.
    QByteArray extended=a;
    extended.replace(72,80,QByteArray(80,0)); extended.replace(96,56,a.mid(72,56));
    put16(extended,52,72); put32(extended,88,20); put16(extended,50,0);
    quint16 vSum=0; for(int i=0;i<72;i+=2) vSum+=qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(extended.constData()+i));
    put16(extended,50,quint16(-vSum));
    CHECK(FixedLayout::replace(extended,96,QByteArray(32,'B'),true,result,error));
    CHECK(result.left(96)==extended.left(96));
    put32(extended,88,24); CHECK(!FixedLayout::replace(extended,96,QByteArray(32,'B'),true,result,error));
    // A sectioned file with a raw leaf: same layout succeeds; malformed / compressed replacement fails.
    a[90]=EFI_FV_FILETYPE_FREEFORM; a[96]=32; a[97]=0; a[98]=0; a[99]=EFI_SECTION_RAW; checksums(a);
    QByteArray body=a.mid(96,32); body[10]='Z';
    CHECK(FixedLayout::replace(a,72,body,true,result,error));
    body[0]=33; CHECK(!FixedLayout::replace(a,72,body,true,result,error));
    body=a.mid(96,32); body[3]=EFI_SECTION_COMPRESSION;
    CHECK(!FixedLayout::replace(a,72,body,true,result,error));
    body[3]=EFI_SECTION_GUID_DEFINED; CHECK(!FixedLayout::replace(a,72,body,true,result,error));
    // Model protection cannot be bypassed by turning off GUI coloring.
    TreeModel m; FfsParser parser(&m); a=fixture(); CHECK(parser.parse(a)==U_SUCCESS);
    auto f=m.findByBase(72); CHECK(FixedLayout::eligibility(a,m,parser,f).isEmpty());
    m.setMarking(f,1); m.setMarkingEnabled(false); CHECK(!FixedLayout::eligibility(a,m,parser,f).isEmpty());
    m.setMarking(f,0); m.setCompressed(f,true); CHECK(!FixedLayout::eligibility(a,m,parser,f).isEmpty());
    const QString source=dir.path()+"/synthetic.fd", target=dir.path()+"/edited.fd";
    write(source,a);
    CHECK(!FixedLayout::saveAs(source,source,a,error));
    CHECK(FixedLayout::saveAs(source,target,a,error)); CHECK(read(target)==a);
    CHECK(!FixedLayout::saveAs(source,target,a,error));
    CHECK(QFile::link(source,dir.path()+"/alias.fd")); CHECK(!FixedLayout::saveAs(source,dir.path()+"/alias.fd",a,error));
    CHECK(!FixedLayout::saveAs(source,dir.path()+"/missing/output.fd",a,error));
    // Deterministically create a competing destination after the initial check.
    const QString raced=dir.path()+"/raced.fd";
    bool publishedHook=false;
    FixedLayout::beforePublishForTest=[&](const QString& path) {
        publishedHook=true;
        CHECK(path==raced); CHECK(!QFileInfo::exists(path));
        write(path,"competitor sentinel");
    };
    const bool raceSaved=FixedLayout::saveAs(source,raced,a,error);
    FixedLayout::beforePublishForTest=nullptr;
    CHECK(publishedHook);
    fprintf(stderr,"Competing destination: saved=%d sentinel_preserved=%d\n",raceSaved,read(raced)=="competitor sentinel");
    CHECK(!raceSaved); CHECK(read(raced)=="competitor sentinel");
    CHECK(!error.isEmpty()); CHECK(read(source)==a);
    const QString unrelated=dir.path()+"/.uefitool-save-unrelated";
    write(unrelated,"not our temporary file");
    const auto stagedFiles=[&]() {
        return QDir(dir.path()).entryList(QStringList()<<".uefitool-save-*",QDir::Files|QDir::Hidden);
    };
    CHECK(stagedFiles()==(QStringList()<<".uefitool-save-unrelated"));
    // A competing directory must also survive publication failure.
    const QString racedDir=dir.path()+"/raced-directory.fd";
    FixedLayout::beforePublishForTest=[&](const QString& path) {
        CHECK(stagedFiles().size()==2); CHECK(QDir().mkdir(path));
        write(path+"/sentinel","directory sentinel");
    };
    CHECK(!FixedLayout::saveAs(source,racedDir,a,error));
    FixedLayout::beforePublishForTest=nullptr;
    CHECK(read(racedDir+"/sentinel")=="directory sentinel");
#ifdef Q_OS_UNIX
    // Real POSIX symlinks (not Windows .lnk shortcuts), including a dangling
    // link and a hard link to the original. Never replace the link or target.
    for(int kind=0;kind<3;++kind) {
        const QString path=dir.path()+QString("/raced-link-%1.fd").arg(kind);
        const QString linkTarget=kind==1 ? dir.path()+"/absent-target.fd" : source;
        FixedLayout::beforePublishForTest=[&](const QString& destination) {
            CHECK(destination==path); CHECK(!QFileInfo::exists(path));
            CHECK(stagedFiles().size()==2);
            if(kind==2) CHECK(::link(QFile::encodeName(linkTarget).constData(),QFile::encodeName(path).constData())==0);
            else CHECK(::symlink(QFile::encodeName(linkTarget).constData(),QFile::encodeName(path).constData())==0);
        };
        CHECK(!FixedLayout::saveAs(source,path,QByteArray("must not replace source"),error));
        FixedLayout::beforePublishForTest=nullptr;
        CHECK(!error.isEmpty()); CHECK(read(source)==a);
        if(kind==2) { CHECK(!QFileInfo(path).isSymLink()); CHECK(read(path)==a); }
        else { CHECK(QFileInfo(path).isSymLink()); CHECK(QFileInfo(path).symLinkTarget()==linkTarget); }
        if(kind==1) CHECK(!QFileInfo::exists(linkTarget));
        CHECK(stagedFiles()==(QStringList()<<".uefitool-save-unrelated"));
    }
#endif
    CHECK(FixedLayout::saveAs(source,dir.path()+"/after-races.fd",a,error));
    CHECK(error.isEmpty()); CHECK(read(dir.path()+"/after-races.fd")==a);
    CHECK(!QFileInfo(dir.path()+"/after-races.fd").isSymLink());
    CHECK(stagedFiles()==(QStringList()<<".uefitool-save-unrelated"));
    CHECK(read(unrelated)=="not our temporary file");
    // Actual GUI model/action/dirty/save/close/open integration, using the same entry points as menu slots.
    UEFITool window; window.show(); window.openImageFile(source);
    auto tree=window.findChild<QTreeView*>("structureTreeView"); CHECK(tree);
    auto model=static_cast<TreeModel*>(tree->model()); tree->setCurrentIndex(model->findByBase(72));
    auto replace=window.findChild<QAction*>("actionReplaceBody"); auto save=window.findChild<QAction*>("actionSaveImageFile");
    CHECK(replace && replace->isEnabled()); CHECK(save && !save->isEnabled());
    write(dir.path()+"/replacement.fbd",QByteArray(32,'B'));
    menuFileAction(replace,dir.path()+"/replacement.fbd");
    CHECK(window.hasUnsavedChanges()); CHECK(save->isEnabled());
    model=static_cast<TreeModel*>(tree->model()); CHECK(model->body(model->findByBase(72))==QByteArray(32,'B'));
    answer(QMessageBox::Cancel); CHECK(!window.close()); CHECK(window.hasUnsavedChanges());
    answer(QMessageBox::Cancel); window.openImageFile(source); CHECK(window.hasUnsavedChanges());
    CHECK(!window.saveEditedImage(source,error)); CHECK(window.hasUnsavedChanges());
    menuFileAction(save,dir.path()+"/gui.fd");
    CHECK(!window.hasUnsavedChanges()); CHECK(read(source)==a);
    CHECK(read(dir.path()+"/gui.fd").mid(96,32)==QByteArray(32,'B'));
    CHECK(window.replaceFileAt(72,QByteArray(32,'C'),true,error));
    answer(QMessageBox::Discard); window.openImageFile(source); CHECK(!window.hasUnsavedChanges());
    model=static_cast<TreeModel*>(tree->model()); CHECK(model->body(model->findByBase(72))==QByteArray(32,'A'));
    tree->setCurrentIndex(model->index(0,0)); CHECK(!replace->isEnabled()); CHECK(window.close());
    printf("PASS: %d checks (synthetic fixtures + offscreen GUI)\n",checks);
    return 0;
}
