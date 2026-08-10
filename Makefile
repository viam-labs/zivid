OUTPUT_NAME := viam-camera-zivid
CONAN_OUTPUT := build-conan
CMAKE_BUILD_DIR := $(CONAN_OUTPUT)/build/Release
BIN_DIR := bin
BINARY := viam-camera-zivid

export CONAN_FLAGS := -s:a build_type=Release -s:a compiler.cppstd=17

.PHONY: setup build conan-build conan-install-test conan-build-test test module.tar.gz clean check-sdk lint

default: build

lint:
ifeq ($(OS),Windows_NT)
	@echo lint unsupported on windows
else
	./bin/run-clang-format.sh
endif

check-sdk:
	@echo "Checking Zivid SDK..."
	@if [ ! -f /usr/include/Zivid/Application.h ]; then \
		echo "ERROR: Zivid SDK not found. Install from: https://www.zivid.com/downloads"; \
		exit 1; \
	fi
	@echo "  Zivid SDK OK ($(shell dpkg -s zivid 2>/dev/null | grep Version | awk '{print $$2}'))"

setup:
	bin/setup.sh
	@$(MAKE) --no-print-directory check-sdk

build:
	@test -f ./venv/bin/activate || $(MAKE) setup
	. ./venv/bin/activate; \
	conan install . \
		--output-folder=$(CONAN_OUTPUT) \
		--build=missing \
		$(CONAN_FLAGS)
	test -f ./venv/bin/activate && . ./venv/bin/activate; \
	cmake --preset conan-release
	test -f ./venv/bin/activate && . ./venv/bin/activate; \
	cmake --build $(CMAKE_BUILD_DIR) --config Release
	@mkdir -p $(BIN_DIR)
	@cp $(CMAKE_BUILD_DIR)/$(BINARY) $(BIN_DIR)/$(BINARY)
	@echo "Binary: $(BIN_DIR)/$(BINARY)"
	@echo "Creating module.tar.gz..."
	tar czf module.tar.gz \
		-C $(BIN_DIR) $(BINARY) install-zivid-sdk.sh install-zivid-tools.sh \
			install-opencl-icd.sh zivid-deb-target.sh \
		-C $(shell pwd) meta.json first_run.sh
	@echo "Created module.tar.gz"

module.tar.gz: build

conan-build: setup build

conan-install-test:
	@test -f ./venv/bin/activate || $(MAKE) setup
	. ./venv/bin/activate; \
	conan install . \
		--output-folder=$(CONAN_OUTPUT) \
		--build=missing \
		-o "&:with_tests=True" \
		$(CONAN_FLAGS)

conan-build-test:
	test -f ./venv/bin/activate && . ./venv/bin/activate; \
	cmake --preset conan-release
	test -f ./venv/bin/activate && . ./venv/bin/activate; \
	cmake --build $(CMAKE_BUILD_DIR) --config Release

test: conan-install-test conan-build-test
	cd $(CMAKE_BUILD_DIR) && . ./generators/conanrun.sh && ctest --output-on-failure

clean:
	rm -rf $(CONAN_OUTPUT) $(BIN_DIR) module.tar.gz venv
	@echo "Clean complete"
