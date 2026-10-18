#!/bin/bash
# Build-configuration checks for Qt discovery (Linux). Nothing is compiled.
# - A qmake6 found only through PATH, such as an active Conda environment's,
#   must not be selected.
# - An explicit QMAKE6 inside an active Conda environment is refused unless
#   QTALH_ALLOW_CONDA=1, which then selects that installation.
# - PKG_CONFIG_PATH must not mix Qt modules from different installations.
# Usage: qtalh/tests/test_build.sh [EPICS_BASE=... EPICS_HOST_ARCH=...]
# Arguments are passed to make; do not pass QT_VERSION, QMAKE6 or PKG_CONFIG.
set -u
QTALH_DIR=$(cd "$(dirname "$0")/.." && pwd)
MAKE=${MAKE:-make}
MAKE_ARGS=("$@")
unset MAKEFLAGS MFLAGS MAKELEVEL
if [ "$(uname -s)" != Linux ]; then
  echo "SKIP: the qmake6 fallback and /usr/bin/pkg-config preference are Linux only"
  exit 0
fi

STUB=$(mktemp -d "${TMPDIR:-/tmp}/qtalh-test-build.XXXXXX") || exit 1
trap 'rm -rf "$STUB"' EXIT
# A Conda-style prefix whose qmake6 reports a complete Qt 6 installation.
mkdir -p "$STUB/bin" "$STUB/lib/qt6" "$STUB/include" "$STUB/lib/pkgconfig"
for mod in Core Gui Widgets Network PrintSupport Multimedia Test; do
  mkdir -p "$STUB/include/Qt$mod"
  : > "$STUB/include/Qt$mod/Qt$mod"
  : > "$STUB/lib/libQt6$mod.so"
done
: > "$STUB/lib/qt6/moc"
: > "$STUB/lib/qt6/rcc"
chmod +x "$STUB/lib/qt6/moc" "$STUB/lib/qt6/rcc"
cat > "$STUB/bin/qmake6" <<EOF
#!/bin/sh
case "\$2" in
  QT_VERSION) echo 6.7.3 ;;
  QT_INSTALL_HEADERS) echo "$STUB/include" ;;
  QT_INSTALL_LIBS) echo "$STUB/lib" ;;
  QT_HOST_LIBEXECS) echo "$STUB/lib/qt6" ;;
  QT_HOST_BINS) echo "$STUB/bin" ;;
esac
EOF
chmod +x "$STUB/bin/qmake6"
# A partial Qt 5 in the same prefix: only Core and Widgets, as a stray
# PKG_CONFIG_PATH entry would provide. The other modules come from the system.
for mod in Core Widgets; do
  cat > "$STUB/lib/pkgconfig/Qt5$mod.pc" <<EOF
prefix=$STUB
libdir=\${prefix}/lib
includedir=\${prefix}/include
host_bins=\${prefix}/bin
Name: Qt5 $mod
Description: stub
Version: 5.15.99
Libs: -L\${libdir} -lQt5$mod
Cflags: -I\${includedir}/Qt$mod
EOF
done

PRINT='print-qt-config: ; @echo "QT_VERSION=$(QT_VERSION)"; echo "QT_CFLAGS=$(QT_CFLAGS)"; echo "QT_LIBS=$(QT_LIBS)"; echo "MOC=$(MOC)"; echo "RCC=$(RCC)"'
# QT6_PKG_AVAILABLE= keeps the qmake6 fallback active on hosts with Qt 6 .pc files.
query() {
  PATH="$STUB/bin:$PATH" CONDA_PREFIX="$STUB" \
    "$MAKE" -s --no-print-directory -C "$QTALH_DIR" QT6_PKG_AVAILABLE= ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"} "$@" \
    --eval="$PRINT" print-qt-config 2>&1
}

failures=0
total=0
pass() { echo "PASS   : $1"; total=$((total + 1)); }
fail() { echo "FAIL!  : $1"; echo "$2" | sed 's/^/   /'; failures=$((failures + 1)); total=$((total + 1)); }

out=$(query)
if [ $? -ne 0 ]; then
  fail "default discovery with a Conda qmake6 on PATH" "$out"
elif echo "$out" | grep -qF "$STUB"; then
  fail "default discovery ignores a Conda qmake6 on PATH" "$out"
else
  pass "default discovery ignores a Conda qmake6 on PATH"
fi

out=$(query QMAKE6="$STUB/bin/qmake6")
if [ $? -ne 0 ] && echo "$out" | grep -qF "active Conda environment"; then
  pass "explicit QMAKE6 into the active Conda environment is refused"
else
  fail "explicit QMAKE6 into the active Conda environment is refused" "$out"
fi

out=$(query QMAKE6="$STUB/bin/qmake6" QTALH_ALLOW_CONDA=1)
if [ $? -eq 0 ] && echo "$out" | grep -qx 'QT_VERSION=6' &&
   echo "$out" | grep -qF -- "-L$STUB/lib -Wl,-rpath,$STUB/lib" &&
   echo "$out" | grep -qx "MOC=$STUB/lib/qt6/moc"; then
  pass "QTALH_ALLOW_CONDA=1 with an explicit QMAKE6 selects that installation"
else
  fail "QTALH_ALLOW_CONDA=1 with an explicit QMAKE6 selects that installation" "$out"
fi

out=$(PKG_CONFIG_PATH="$STUB/lib/pkgconfig" query QT_VERSION=5 QTALH_ALLOW_CONDA=1)
if [ $? -ne 0 ] && echo "$out" | grep -qF "different installations"; then
  pass "PKG_CONFIG_PATH mixing two Qt 5 installations is refused"
else
  fail "PKG_CONFIG_PATH mixing two Qt 5 installations is refused" "$out"
fi

echo "Totals: $((total - failures)) passed, $failures failed"
[ "$failures" -eq 0 ]
