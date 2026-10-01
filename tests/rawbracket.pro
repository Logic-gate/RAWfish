TEMPLATE = app
TARGET = tst_rawbracket
QT += core gui testlib
CONFIG += testcase console c++14
SOURCES += rawbracket.cpp ../src/rawbracket.cpp ../src/imageadjustments.cpp
HEADERS += ../src/rawbracket.h ../src/rawbracketmath.h ../src/imageadjustments.h
