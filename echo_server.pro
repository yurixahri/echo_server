QT = core network sql

CONFIG += c++20 cmdline

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0
QMAKE_LIBS += -ltasn1 -lz

SOURCE_SQL_DIR = $$PWD/sql
SOURCE_ASN1_DEF = $$PWD/ChatProtocol.asn
CONFIG(debug, debug|release) {
    DEST_SQL_DIR = $$OUT_PWD/debug/sql
    DEST_DIR = $$OUT_PWD/debug
} else {
    DEST_SQL_DIR = $$OUT_PWD/release/sql
    DEST_DIR = $$OUT_PWD/release
}

win32 {
    SOURCE_SQL_DIR ~= s,/,\\,g
    DEST_SQL_DIR   ~= s,/,\\,g
    DEST_DIR ~= s,/,\\,g
    SOURCE_ASN1_DEF ~= s,/,\\,g
}


copy_sql.commands = $(COPY_DIR) $$SOURCE_SQL_DIR $$DEST_SQL_DIR
copy_asn1.commands = $(COPY_FILE) $$SOURCE_ASN1_DEF $$DEST_DIR

QMAKE_EXTRA_TARGETS += copy_asn1 copy_sql
POST_TARGETDEPS += copy_asn1 copy_sql

SOURCES += \
        main.cpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

HEADERS += \
    db/db.h \
    common/types.h \
    db/user.h \
    functions/user.h \
    utils/asn1_parser.h \
    utils/zlib_helper.h \
