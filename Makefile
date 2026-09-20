# Makefile per compilare agssetup (C++/Qt6)
#   make            - compila con qmake6 (nella cartella build/)
#   make cmake      - compila con CMake  (nella cartella build-cmake/)
#   make clean      - rimuove le cartelle di build
#
# Nota: qmake genera un proprio "Makefile" nella cartella da cui viene lanciato.
# Per questo la compilazione avviene in una sottocartella: lanciarlo qui
# sovrascriverebbe questo file.

.PHONY: all cmake clean

all:
	mkdir -p build
	cd build && qmake6 ../agssetup.pro && $(MAKE)

cmake:
	mkdir -p build-cmake
	cd build-cmake && cmake .. && $(MAKE)

clean:
	rm -rf build build-cmake
