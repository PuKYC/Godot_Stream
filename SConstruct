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
# 移除原来的 CCFLAGS 设置，改为后面统一处理
# localEnv.Append(CCFLAGS=['-std=c++20', '-DLIBUUID_CPP20_OR_GREATER'])  # <-- 删除

# Build profiles ...
customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)

options = [
    CompileTimeOption(
        key="enable_fts5",
        name="FTS5",
        help="Enable SQLite's FTS5 extension...",
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

if env.get("platform") == "web" and env.get("threads", "") != "yes":
    env["threads"] = "no"

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available...""")
    sys.exit(1)

# 调用 godot-cpp 的 SConstruct，它会根据平台设置基础编译环境
env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

# ===== 在 SConscript 之后，根据平台应用 C++20 标准和宏定义 =====
if env['platform'] == 'windows':
    # MSVC：使用 /std:c++20
    env.Append(CCFLAGS=['/std:c++20'])
    # 移除可能残留的 -std=c++20 和 -fexceptions
    env['CCFLAGS'] = [f for f in env['CCFLAGS'] if f not in ('-std=c++20', '-fexceptions')]
else:
    # 其他平台（Linux, macOS, Android, iOS 等）使用 -std=c++20
    env.Append(CCFLAGS=['-std=c++20'])
    # 对于需要异常支持的第三方代码，保留 -fexceptions（除非平台明确不需要）
    # 但注意：Android 等可能需要额外处理，这里简单添加
    if '-fexceptions' not in env['CCFLAGS']:
        env.Append(CCFLAGS=['-fexceptions'])

# 添加宏定义（跨平台使用 CPPDEFINES）
env.Append(CPPDEFINES=['LIBUUID_CPP20_OR_GREATER'])

# ===== SQLite 相关宏定义（根据选项） =====
for opt in options:
    if env.get(opt.key, False):
        env.Append(CPPDEFINES=[opt.define])

# 强制编译 sqlite_rtree
env.Append(CPPDEFINES=["SQLITE_ENABLE_RTREE"])

# 添加头文件路径
env.Append(CPPPATH=[
    "src",
]) 

# 源文件列表
sources = []
sources += Glob("src/*.cpp")
sources += Glob("src/core/stream/*.cpp")
sources += Glob("src/core/components/*.cpp")
sources += Glob("gdsqlite/*.cpp")
sources += Glob("gdsqlite/sqlite/*.c")
sources += Glob("gdsqlite/vfs/*.cpp")

# 文档生成（如果需要）
if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

# 处理 Android 平台链接标志（保持不变）
if env['platform'] == 'android':
    darwin_flags = ['-Wl,-dead_strip', '-dead_strip_dylibs', '-no_warn_duplicate_libraries', '-dynamic', '-dylib']
    for flag in darwin_flags:
        while flag in env.get('LINKFLAGS', []):
            env['LINKFLAGS'].remove(flag)
    env.Append(LINKFLAGS=['-Wl,--gc-sections'])

# 构建库
suffix = env['suffix'].replace(".dev", "").replace(".universal", "")
lib_filename = "{}{}{}{}".format(env.subst('$SHLIBPREFIX'), libname, suffix, env.subst('$SHLIBSUFFIX'))

library = env.SharedLibrary(
    "bin/{}/{}".format(env['platform'], lib_filename),
    source=sources,
)

copy = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

default_args = [library, copy]
Default(*default_args)