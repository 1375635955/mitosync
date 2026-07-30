#!/bin/bash
set -e

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build-cov}"
REPORT_DIR="${REPORT_DIR:-coverage-html}"
VCPKG_TOOLCHAIN="${VCPKG_TOOLCHAIN:-vcpkg/scripts/buildsystems/vcpkg.cmake}"

# Check for required tools
missing_tools=()

command -v lcov &> /dev/null || missing_tools+=("lcov")
command -v genhtml &> /dev/null || missing_tools+=("genhtml")
command -v python3 &> /dev/null || missing_tools+=("python3")
command -v cmake &> /dev/null || missing_tools+=("cmake")

if [ ${#missing_tools[@]} -ne 0 ]; then
    echo "Error: Missing required tools: ${missing_tools[*]}"
    echo ""
    echo "Install with:"
    echo "  brew install lcov cmake python3"
    exit 1
fi

# Configure if needed
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
    echo "==> Configuring coverage build..."
    cmake -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN" \
        -DCOVERAGE=ON \
        -DBUILD_GUI=OFF
fi

# Build
echo "==> Building..."
cmake --build "$BUILD_DIR"

# Run tests
echo "==> Running tests..."
ctest --test-dir "$BUILD_DIR" --output-on-failure

# Generate coverage
echo "==> Generating coverage report..."
lcov --capture --directory "$BUILD_DIR" -o coverage.raw.info \
    --ignore-errors inconsistent,mismatch,gcov,format,unsupported,negative

# Filter to only project source files
echo "==> Filtering coverage data..."
python3 scripts/filter_coverage.py coverage.raw.info coverage.info || {
    echo "Error: Failed to filter coverage"
    exit 1
}
rm -f coverage.raw.info

# Generate HTML
genhtml coverage.info -o "$REPORT_DIR" --quiet \
    --ignore-errors inconsistent,corrupt,unsupported,category,empty 2>/dev/null || true

# Summary
echo ""
echo "==> Coverage Summary:"
lcov --summary coverage.info 2>&1 | grep -E "lines|functions"
echo ""
echo "Report: $REPORT_DIR/index.html"

# Open report (macOS)
if [ "$(uname)" = "Darwin" ]; then
    open "$REPORT_DIR/index.html"
fi
