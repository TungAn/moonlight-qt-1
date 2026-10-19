QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle

TARGET = udpprobe-test

INCLUDEPATH += ..

SOURCES += udpprobe_test.cpp
HEADERS += ../backend/udpprobe.h
