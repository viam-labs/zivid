CONAN_FLAGS := -s:a build_type=Release -s:a compiler.cppstd=17
BUILD_DIR := build/Release

.PHONY: default conan-install configure build install module.tar.gz clean \
        docker-amd64 docker-run

default: module.tar.gz

conan-install:
	conan install . \
		--output-folder=. \
		--build=missing \
		$(CONAN_FLAGS)

configure: conan-install
	cmake --preset conan-release

build: configure
	cmake --build --preset conan-release -j$$(nproc)

install: build
	cmake --install $(BUILD_DIR)

module.tar.gz: build
	cmake --build $(BUILD_DIR) --target package
	@echo "Built $(BUILD_DIR)/module.tar.gz"

clean:
	rm -rf build build-conan CMakeUserPresets.json module.tar.gz

#
# Docker — build environment image for linux/amd64, then run `make` inside.
# Persistent named volumes cache conan deps and ccache between runs so
# subsequent builds skip rebuilding Boost / gRPC / viam-cpp-sdk / etc.
#
IMAGE := viam-camera-zivid-build:amd64
DOCKERFILE := Dockerfile
CONAN_VOLUME := viam-zivid-conan
CCACHE_VOLUME := viam-zivid-ccache

docker-amd64:
	docker buildx build --pull --load --platform linux/amd64 \
		-f $(DOCKERFILE) -t $(IMAGE) .

docker-run: docker-amd64
	docker run --rm --platform linux/amd64 \
		-v $(PWD):/src -w /src \
		-v $(CONAN_VOLUME):/root/.conan2 \
		-v $(CCACHE_VOLUME):/root/.ccache \
		$(IMAGE) make module.tar.gz

docker-shell: docker-amd64
	docker run --rm -it --platform linux/amd64 \
		-v $(PWD):/src -w /src \
		-v $(CONAN_VOLUME):/root/.conan2 \
		-v $(CCACHE_VOLUME):/root/.ccache \
		$(IMAGE) bash

docker-clean-cache:
	docker volume rm $(CONAN_VOLUME) $(CCACHE_VOLUME) || true
