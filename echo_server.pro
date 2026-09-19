QT = core network sql

CONFIG += c++20 cmdline

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0
QMAKE_LIBS += -ltasn1 -lz

SOURCE_DIR = $$quote($$PWD/sql)
CONFIG(debug, debug|release) {
    # Path for Debug mode
    DEST_DIR = $$OUT_PWD/debug/sql
} else {
    # Path for Release mode
    DEST_DIR = $$OUT_PWD/release/sql
}

win32 {
    SOURCE_DIR ~= s,/,\\,g
    DEST_DIR   ~= s,/,\\,g
}

win32 {
    copy.commands = xcopy /s /q /y /i $$quote($$SOURCE_DIR) $$quote($$DEST_DIR)
} else {
    copy.commands = $(MKDIR) $$quote($$DEST_DIR) && $(COPY_DIR) $$quote($$SOURCE_DIR) $$quote($$DEST_DIR)
}

copy.depends = $(first)
QMAKE_EXTRA_TARGETS += copy
PRE_TARGETDEPS += copy

SOURCES += \
        main.cpp

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

HEADERS += \
    db/db.h \
    utils/asn1_parser.h \
    utils/zlib_helper.h
