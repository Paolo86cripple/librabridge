# Makefile per compilare agssetup (C++/Qt6)
# Opzioni:
#   make            - Compila con qmake6
#   make clean      - Pulisce i file generati
#   make cmake     - Compila con CMake (richiede cmake)

.PHONY: all clean cmake

all: agssetup

agssetup:
	qmake6 -project -o agssetup.pro.tmp 2>/dev/null || true
	qmake6 agssetup.pro
	$(MAKE) -C $(shell pwd)

clean:
	rm -rf agssetup.pro.tmp agssetup.o agssetup.moc agssetup Makefile *.o moc_*.cpp

cmake:
	mkdir -p build && cd build && cmake .. && make

# Fine Makefile
