TARGET = qtposition_luneos
QT = core positioning

PLUGIN_TYPE = position
PLUGIN_CLASS_NAME = QGeoPositionInfoSourceFactoryLuneOS
load(qt_plugin)

HEADERS += \
    qgeopositioninfosource_luneos_p.h \
    qgeopositioninfosourcefactory_luneos.h

SOURCES += \
    qgeopositioninfosource_luneos.cpp \
    qgeopositioninfosourcefactory_luneos.cpp

CONFIG += link_pkgconfig
PKGCONFIG += luna-service2
PKGCONFIG += glib-2.0

CONFIG += exceptions
CONFIG += no_keywords
LIBS += -lluna-service2++

OTHER_FILES += plugin.json
