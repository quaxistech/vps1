.PHONY: configure build test unit package clean

configure:
	cmake --preset release

build: configure
	cmake --build --preset release

test: build
	ctest --preset release

unit: build
	ctest --preset release -L unit

package: test
	./scripts/build-release.sh

clean:
	rm -rf build dist

