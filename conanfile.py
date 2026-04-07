import os
import tarfile
import re
from tempfile import TemporaryDirectory

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load


class ViamZivid(ConanFile):
    name = "viam-camera-zivid"
    license = "Apache-2.0"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {
        "with_tests": False,
        "viam-cpp-sdk/*:shared": False,
    }

    exports_sources = "CMakeLists.txt", "src/*", "etc/meta.json"

    version = "0.1.0"

    def set_version(self):
        content = load(self, "CMakeLists.txt")
        version_match = re.search(r"set\(CMAKE_PROJECT_VERSION (.+)\)", content)
        if version_match:
            self.version = version_match.group(1).strip()

    def validate(self):
        check_min_cppstd(self, 17)

    def requirements(self):
        self.requires("viam-cpp-sdk/0.33.1")
        self.requires("stb/cci.20230920")

    def layout(self):
        cmake_layout(self, src_folder=".")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def deploy(self):
        with TemporaryDirectory(dir=self.deploy_folder) as tmp_dir:
            self.output.info("Deploying files to module.tar.gz")

            copy(self, "viam-camera-zivid", src=self.package_folder, dst=tmp_dir)
            copy(self, "meta.json", src=self.package_folder, dst=tmp_dir)

            self.output.info("Creating module.tar.gz")
            with tarfile.open(os.path.join(self.deploy_folder, "module.tar.gz"), "w|gz") as tar:
                tar.add(tmp_dir, arcname=".", recursive=True)
