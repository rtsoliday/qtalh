#pragma once
#include <QtTest>

// Qt 5.15 and early Qt 6 releases only provide the older assertion name.
#ifndef QVERIFY_THROWS_EXCEPTION
#define QVERIFY_THROWS_EXCEPTION(exceptionType, ...) \
  QVERIFY_EXCEPTION_THROWN((__VA_ARGS__), exceptionType)
#endif
