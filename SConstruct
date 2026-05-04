#!/usr/bin/env python

import os
import sys

from methods import print_error


class CompileTimeOption:
    def __init__(self, key, name, help, define):
        self.key = key
        self.name = name
        self.help = help
        self.define = define


libname = "StreamWorld"
projectdir = "project"

localEnv = Environment(tools=["default"], PLATFORM="")

# Build profiles can be used to decrease compile times.
# You can either specify "disabled_classes", OR
# explicitly specify "enabled_classes" which disables all other classes.
# Modify the example file as needed and uncomment the line below or
# manually specify the build_profile parameter when running SCons.
# localEnv["build_profile"] = "build_profile.json"

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)

# ===== SQLite 编译选项 =====
options = [
    CompileTimeOption(
        key="enable_fts5",
        name="FTS5",
        help="Enable SQLite's FTS5 extension which provides full-text search functionality to database applications",
        define="SQLITE_ENABLE_FTS5",
    ),
    CompileTimeOption(
        key="enable_math_functions",
        name="MATH_FUNCTIONS",
        help="Enable SQLite's Built-in Mathematical SQL Functions",
        define="SQLITE_ENABLE_MATH_FUNCTIONS",
    ),
]

for opt in options:
    opts.Add(BoolVariable(opt.key, opt.help, False))

opts.Update(localEnv)
Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error(
        """godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:
    git submodule update --init --recursive"""
    )
    sys.exit(1)

env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

# ===== C++ 标准 =====
# 必须在 godot-cpp/SConstruct 之后设置，且只作用于 C++ 文件（CXXFLAGS）。
# MSVC 使用 /std:c++20，GCC / Clang / Emscripten 使用 -std=c++20。
if env.get("is_msvc", False):
    env.Append(CXXFLAGS=["/std:c++20"])
else:
    env.Append(CXXFLAGS=["-std=c++20"])

# ===== 异常处理 =====
# Android NDK 和 Emscripten 默认关闭 C++ 异常，需显式启用。
# 其他平台（Linux / macOS / Windows）默认启用，无需额外设置。
if env["platform"] in ["android", "web"]:
    env.Append(CXXFLAGS=["-fexceptions"])

# ===== 链接器优化 =====
# --gc-sections 消除未使用的代码段，仅 GNU ld（Linux / Android）支持。
# macOS 使用 -dead_strip（由 godot-cpp 负责），Windows / Web 不需要此 flag。
if env["platform"] in ["linux", "android"]:
    # 清理可能从 godot-cpp 继承的 macOS 专用 linker flag
    _darwin_flags = [
        "-Wl,-dead_strip",
        "-dead_strip_dylibs",
        "-no_warn_duplicate_libraries",
        "-dynamic",
        "-dylib",
    ]
    for _flag in _darwin_flags:
        while _flag in env.get("LINKFLAGS", []):
            env["LINKFLAGS"].remove(_flag)
    env.Append(LINKFLAGS=["-Wl,--gc-sections"])

# ===== 根据用户选项添加宏定义 =====
for opt in options:
    if env.get(opt.key, False):
        env.Append(CPPDEFINES=[opt.define])

env.Append(CPPPATH=["src","src/gdsqlite"])

# 强制启用 SQLite R*Tree 模块
env.Append(CPPDEFINES=["SQLITE_ENABLE_RTREE"])

# ===== 源文件 =====
sources = []
sources += Glob("src/*.cpp")
sources += Glob("src/core/components/*.cpp")
sources += Glob("src/core/stream/*.cpp")

# ===== gdsqlite 源文件 =====
# macOS universal 构建时，对 gdsqlite 下所有文件（.cpp 和 .c）
# 显式注入 -arch x86_64 -arch arm64。
#
# 根因：godot-cpp 对 universal 的 arch flag 处理方式未必覆盖用户源文件，
# 导致 gdsqlite 里的 C++ 和 C 文件都只编译出 arm64 切片。
# 同时 CI SCons 缓存可能保留了旧的 arm64-only 对象，
# 用独立的 env.Clone() + 显式 CCFLAGS 可让 SCons 识别为新签名，强制重编。
_gdsqlite_cpp = Glob("src/gdsqlite/*.cpp") + Glob("src/gdsqlite/vfs/*.cpp")
_gdsqlite_c   = Glob("src/gdsqlite/sqlite/*.c")

if env["platform"] == "macos" and env.get("arch", "") == "universal":
    _gdsqlite_env = env.Clone()
    # CCFLAGS 同时作用于 C 和 C++ 文件
    _gdsqlite_env.Append(CCFLAGS=["-arch", "x86_64", "-arch", "arm64"])
    sources += [_gdsqlite_env.SharedObject(f) for f in _gdsqlite_cpp + _gdsqlite_c]
else:
    sources += _gdsqlite_cpp
    sources += _gdsqlite_c

# ===== 文档数据（仅 debug / editor 目标）=====
if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData(
            "src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml")
        )
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

# ===== 构建输出 =====
# .dev 不影响兼容性，.universal 表示多架构合并，两者都不需要体现在文件名中。
suffix = env["suffix"].replace(".dev", "").replace(".universal", "")
lib_filename = "{}{}{}{}".format(
    env.subst("$SHLIBPREFIX"), libname, suffix, env.subst("$SHLIBSUFFIX")
)

library = env.SharedLibrary(
    "bin/{}/{}".format(env["platform"], lib_filename),
    source=sources,
)

copy = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

Default(library, copy)
