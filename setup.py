# Copyright 2024 Tencent Inc.  All rights reserved.
#
# ==============================================================================

import os
import pathlib

from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext as build_ext_orig
from distutils.file_util import copy_file


class CMakeExtension(Extension):

    def __init__(self, name):
        # don't invoke the original build_ext for this special extension
        super().__init__(name, sources=[])


class build_ext(build_ext_orig):

    def run(self):
        for ext in self.extensions:
            self.build_cmake(ext)
        super().run()

    def build_cmake(self, ext):
        cwd = pathlib.Path().absolute()

        # these dirs will be created in build_py, so if you don't have
        # any python sources to bundle, the dirs will be missing
        build_temp = pathlib.Path(self.build_temp)
        build_temp.mkdir(parents=True, exist_ok=True)
        extdir = pathlib.Path(self.get_ext_fullpath(ext.name))
        if not extdir.exists():
            extdir.mkdir(parents=True, exist_ok=True)

        # example of cmake args
        config = 'Debug' if self.debug else 'Release'
        cmake_args = [
            '-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=' +
            str(extdir.parent.absolute()), '-DCMAKE_BUILD_TYPE=' + config,
            '-DWITH_TESTING=ON'
        ]

        # example of build args
        build_args = ['--config', config, '--', '-j']

        os.chdir(str(build_temp))
        self.spawn(['cmake', str(cwd)] + cmake_args)
        if not self.dry_run:
            self.spawn(['cmake', '--build', '.'] + build_args)
        # Troubleshooting: if fail on line above then delete all possible
        # temporary CMake files including "CMakeCache.txt" in top level dir.
        os.chdir(str(cwd))
        # cp all temp dir's lib to cwd for dist package
        deps_lib = cwd.joinpath('src/numerous_llm/python/lib')
        build_temp_lib = build_temp.joinpath('lib')
        deps_lib.mkdir(parents=True, exist_ok=True)
        target_libs = ["libtorch_serving.so", "libloguru.so"]
        for target_lib in target_libs:
            # for local develop
            copy_file(str(build_temp_lib.joinpath(target_lib)), str(deps_lib))
            # for wheel pacakge
            copy_file(str(build_temp_lib.joinpath(target_lib)), str(extdir.parent.absolute()))


setup(name='numerous_llm',
      version='0.1',
      author='numerous',
      author_email='karlluo@tencent.com',
      description='Numerous LLM inference server',
      platforms='python3',
      url='https://git.woa.com/RondaServing/LLM/NumerousLLM/',
      packages=find_packages('src/numerous_llm/python'),
      package_dir={
          '': 'src/numerous_llm/python',
      },
      ext_modules=[CMakeExtension('numerous_llm')],
      python_requires='>=3',
      cmdclass={
          'build_ext': build_ext,
      })