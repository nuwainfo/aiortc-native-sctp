#!/usr/bin/env python
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# FastFileLink CLI - Fast, no-fuss file sharing
# Copyright (C) 2025-2026 FastFileLink contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


class UsrsctpBuildExt(build_ext):
    def build_extensions(self):
        root_value = os.environ.get("USRSCTP_ROOT")
        if not root_value:
            raise RuntimeError(
                "USRSCTP_ROOT is not set. Build/install usrsctp first, then set "
                "USRSCTP_ROOT to its install prefix."
            )

        root = Path(root_value).resolve()
        include_dirs = [str(root / "include")]
        library_dirs = [str(root / "lib"), str(root / "lib64")]
        library_name = os.environ.get("USRSCTP_LIBNAME", "usrsctp")

        for ext in self.extensions:
            ext.include_dirs = include_dirs
            ext.library_dirs = library_dirs
            ext.libraries = [library_name]

            if sys.platform == "win32":
                ext.libraries += ["ws2_32", "iphlpapi"]
                ext.define_macros += [
                    ("_CRT_SECURE_NO_WARNINGS", "1"),
                    ("WIN32_LEAN_AND_MEAN", "1"),
                    ("NOMINMAX", "1"),
                ]
                ext.extra_compile_args += ["/O2"]
            else:
                ext.define_macros += [("_CRT_SECURE_NO_WARNINGS", "1")]
                ext.extra_compile_args += ["-O3"]

        super().build_extensions()


setup(
    ext_modules=[
        Extension(
            "aiortc_native_sctp._native_sctp",
            sources=["src/aiortc_native_sctp/_native_sctp.c"],
            define_macros=[],
            extra_compile_args=[],
        )
    ],
    cmdclass={"build_ext": UsrsctpBuildExt},
)
