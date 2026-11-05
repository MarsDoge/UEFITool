// Optional test harness using exactly the GUI's fixed-layout engine.
#include "../common/fixedlayout.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>
static void list(const QByteArray& image,const TreeModel& m,const FfsParser& p,const QModelIndex& root=QModelIndex()) {
    for(int i=0;i<m.rowCount(root);++i) {
        auto n=m.index(i,0,root);
        if(m.type(n)==Types::File) {
            QString why=FixedLayout::eligibility(image,m,p,n);
            printf("0x%08x %u %s %s\n",m.base(n),m.fullSize(n),qPrintable(m.name(n)),why.isEmpty()?"EDITABLE":qPrintable(why));
        }
        list(image,m,p,n);
    }
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    if(argc!=3 && argc!=6) { fprintf(stderr,"Usage: %s list IMAGE | %s body|file IMAGE OFFSET REPLACEMENT NEW_OUTPUT\n",argv[0],argv[0]); return 2; }
    QFile input(argv[2]); if(!input.open(QIODevice::ReadOnly)) return 2;
    QByteArray image=input.readAll();
    if(QString(argv[1])=="list" && argc==3) { TreeModel m; FfsParser p(&m); if(p.parse(image)!=U_SUCCESS)return 1; list(image,m,p); return 0; }
    if(argc!=6 || (QString(argv[1])!="body" && QString(argv[1])!="file"))return 2;
    bool ok=false; quint32 offset=QString(argv[3]).toUInt(&ok,0); if(!ok)return 2;
    QFile replacement(argv[4]); if(!replacement.open(QIODevice::ReadOnly))return 2;
    QByteArray output; QString error;
    if(!FixedLayout::replace(image,offset,replacement.readAll(),QString(argv[1])=="body",output,error) ||
       !FixedLayout::saveAs(argv[2],argv[5],output,error)) { fprintf(stderr,"Rejected: %s\n",qPrintable(error)); return 1; }
    printf("Saved and verified %d bytes. Experimental structural validation only.\n",output.size()); return 0;
}
