QT += core qml network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TARGET = settingsapi-test
INCLUDEPATH += ..
SOURCES += settingsapi_test.cpp ../cli/settingsapi.cpp ../settings/streamingpreferences.cpp ../streaming/vrrratepolicy.cpp
HEADERS += ../cli/settingsapi.h ../settings/streamingpreferences.h ../settings/bitratecalculator.h ../settings/vrrtimingoptions.h ../streaming/vrrratepolicy.h ../utils.h
