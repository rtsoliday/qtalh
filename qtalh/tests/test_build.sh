#!/bin/bash
# Build-configuration checks for Qt discovery (Linux). Nothing is compiled.
# - A qmake6 found only through PATH must not be selected.
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
# A qmake6 on PATH should be ignored in favor of the selected installation.
mkdir -p "$STUB/bin" "$STUB/lib/pkgconfig"
ln -s "$(command -v false)" "$STUB/bin/qmake6"
# A partial Qt 5 in the same prefix: only Core and Widgets, as a stray
# PKG_CONFIG_PATH entry would provide. Keep the real code generators available
# so the mixed-prefix check, rather than a missing-tool check, reports the error.
QT5_HOST_BINS=$(/usr/bin/pkg-config --variable=host_bins Qt5Core 2>/dev/null)
QT5_LIBEXECDIR=$(/usr/bin/pkg-config --variable=libexecdir Qt5Core 2>/dev/null)
for mod in Core Widgets; do
  cat > "$STUB/lib/pkgconfig/Qt5$mod.pc" <<EOF
prefix=$STUB
libdir=\${prefix}/lib
includedir=\${prefix}/include
host_bins=$QT5_HOST_BINS
libexecdir=$QT5_LIBEXECDIR
Name: Qt5 $mod
Description: stub
Version: 5.15.99
Libs: -L\${libdir} -lQt5$mod
Cflags: -I\${includedir}/Qt$mod
EOF
done

PRINT='print-qt-config: ; @echo "QT_VERSION=$(QT_VERSION)"; echo "QT_CFLAGS=$(QT_CFLAGS)"; echo "QT_LIBS=$(QT_LIBS)"; echo "MOC=$(MOC)"; echo "RCC=$(RCC)"; echo "QMAKE=$(QMAKE)"'
query() {
  PATH="$STUB/bin:$PATH" \
    "$MAKE" -s --no-print-directory -C "$QTALH_DIR" ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"} "$@" \
    --eval="$PRINT" print-qt-config 2>&1
}

failures=0
total=0
pass() { echo "PASS   : $1"; total=$((total + 1)); }
fail() { echo "FAIL!  : $1"; echo "$2" | sed 's/^/   /'; failures=$((failures + 1)); total=$((total + 1)); }

out=$(query)
if [ $? -ne 0 ]; then
  fail "default discovery with a qmake6 on PATH" "$out"
elif echo "$out" | grep -qF "$STUB"; then
  fail "default discovery ignores a qmake6 on PATH" "$out"
else
  pass "default discovery ignores a qmake6 on PATH"
fi

if /usr/bin/pkg-config --exists Qt5Core Qt5Gui Qt5Widgets Qt5Network Qt5PrintSupport Qt5Multimedia; then
  out=$(PKG_CONFIG_PATH="$STUB/lib/pkgconfig" query QT_VERSION=5)
  if [ $? -ne 0 ] && echo "$out" | grep -qF "different installations"; then
    pass "PKG_CONFIG_PATH mixing two Qt 5 installations is refused"
  else
    fail "PKG_CONFIG_PATH mixing two Qt 5 installations is refused" "$out"
  fi
else
  echo "SKIP: mixed Qt 5 installation check requires system Qt 5 modules"
fi

# Optional SDDS: the probe also runs when cleanup precedes a build, and a
# library directory holding only static archives adds no run-time search path.
# Dry runs: the clean recipe is printed, not run.
SDDS_PRINT='print-sdds: ; @echo "SDDS_SELECTED=$(SDDS_SELECTED)|SDDS_DIR=$(sdds_library_$(SDDS_SELECTED))|SDDS_LINK_LIBS=$(SDDS_LINK_LIBS)|"'
sdds_query() {
  "$MAKE" -n -s --no-print-directory -C "$QTALH_DIR" ${MAKE_ARGS[@]+"${MAKE_ARGS[@]}"} "$@" \
    --eval="$SDDS_PRINT" print-sdds 2>&1 | sed -n 's/.*SDDS_SELECTED=\([^|]*\)|SDDS_DIR=\([^|]*\)|SDDS_LINK_LIBS=\([^|]*\)|.*/\1|\2|\3/p'
}
sdds=$(sdds_query)
selected=${sdds%%|*}
if [ -z "$selected" ]; then
  echo "SKIP: SDDS checks require a usable SDDS installation"
else
  cleaned=$(sdds_query clean)
  if [ "${cleaned%%|*}" = "$selected" ]; then
    pass "cleanup before a build keeps SDDS export"
  else
    fail "cleanup before a build keeps SDDS export" "selected: '$selected', after clean: '${cleaned%%|*}'"
  fi
  rest=${sdds#*|}
  dir=${rest%%|*}
  links=${rest#*|}
  if ls "$dir"/lib*.so* "$dir"/lib*.dylib >/dev/null 2>&1; then
    echo "SKIP: $dir holds shared libraries, so its run-time search path is needed"
  elif echo "$links" | grep -qF -- "-rpath,$dir"; then
    fail "static SDDS libraries add no run-time search path" "$links"
  else
    pass "static SDDS libraries add no run-time search path"
  fi
fi

echo "Totals: $((total - failures)) passed, $failures failed"
[ "$failures" -eq 0 ]
