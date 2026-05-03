#!/usr/bin/env python
import os
import sys

from methods import print_error

# 如果 CompileTimeOption 未定义，请在此处定义
class CompileTimeOption:
    def __init__(self, key, name, help, define):
        self.key = key
        self.name = name
        self.help = help
        self.define = define

libname = "StreamWorld"
projectdir = "project"

localEnv = Environment(tools=["default"], PLATFORM="")
localEnv.Append(CCFLAGS=['-std=c++20', '-DLIBUUID_CPP20_OR_GREATER'])

# Build profiles can be used to decrease compile times.
# You can either specify "disabled_classes", OR
# explicitly specify "enabled_classes" which disables all other classes.
# Modify the example file as needed and uncomment the line below or
# manually specify the build_profile parameter when running SCons.

# localEnv["build_profile"] = "build_profile.json"

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)

# ===== 添加 SQLite 编译选项 =====
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
# ===============================

opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

# 针对 Web 平台自动禁用线程，避免 LLVM 异常模型冲突
if env.get("platform") == "web" and env.get("threads", "") != "yes":
    env["threads"] = "no"

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:

    git submodule update --init --recursive""")
    sys.exit(1)

env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

# ===== 根据用户选择添加宏定义 =====
for opt in options:
    if env.get(opt.key, False):
        env.Append(CPPDEFINES=[opt.define])
# ================================

env.Append(CPPPATH=[
    "src",
]) 

# 强制编译 sqlite_rtree 模块
env.Append(CPPDEFINES=["SQLITE_ENABLE_RTREE"])

sources = []
sources += Glob("src/*.cpp")
sources += Glob("src/core/stream/*.cpp")
sources += Glob("src/core/components/*.cpp")

sources += Glob("gdsqlite/*.cpp") 
sources += Glob("gdsqlite/sqlite/*.c")
sources += Glob("gdsqlite/vfs/*.cpp")

# 第三方代码包含异常但必须使用
env.Append(CXXFLAGS=['-fexceptions'])

if env['platform'] == 'android':
    darwin_flags = ['-Wl,-dead_strip', '-dead_strip_dylibs', '-no_warn_duplicate_libraries', '-dynamic', '-dylib']

    for flag in darwin_flags:
        while flag in env.get('LINKFLAGS', []):
            env['LINKFLAGS'].remove(flag)

    env.Append(LINKFLAGS=['-Wl,--gc-sections'])

if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

# .dev doesn't inhibit compatibility, so we don't need to key it.
# .universal just means "compatible with all relevant arches" so we don't need to key it.
suffix = env['suffix'].replace(".dev", "").replace(".universal", "")

lib_filename = "{}{}{}{}".format(env.subst('$SHLIBPREFIX'), libname, suffix, env.subst('$SHLIBSUFFIX'))

library = env.SharedLibrary(
    "bin/{}/{}".format(env['platform'], lib_filename),
    source=sources,
)

copy = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

default_args = [library, copy]
Default(*default_args)