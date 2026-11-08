project('nemu', 'c',
        default_options : ['buildtype=debugoptimized'])

source_files = [
  'src/bitmath.c',
  'src/systems/sst.c',
  'src/systems/famicom.c',
  'src/systems/mappers.c',
  'src/systems/fds.c',
  'src/chips/n163.c',
  'src/chips/vrc6.c',
  'src/chips/vrc7.c',
  'src/zip.c',
  'src/audio.c',
  'src/input.c',
  'src/chips/6502.c',
  'src/chips/2C02.c',
  'src/graphics.c',
  'src/nemu.c'
]

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)
raylib_dep = cc.find_library('raylib', required : true)
threads_dep = dependency('threads')
#xlib_dep = dependency('x11')

deps = [m_dep,
        raylib_dep,
        threads_dep,
#        xlib_dep,
       ]

if host_machine.system() == 'windows'
  # the video export reads frames back through the driver's own opengl calls
  deps += cc.find_library('opengl32', required : true)
  windows = import('windows')
  source_files += windows.compile_resources('resources/windows/nemu.rc',
                                            include_directories : include_directories('resources/windows'),
                                            depend_files : 'resources/windows/nemu.manifest')
endif

executable('nemu',
           source_files,
           dependencies: deps
          )

# copy the toolchain dlls next to the exe so the build folder runs anywhere
if host_machine.system() == 'windows'
  fs = import('fs')
  toolchain_bin = fs.parent(find_program(cc.cmd_array()[0]).full_path())
  foreach dll : ['libraylib.dll', 'glfw3.dll', 'libwinpthread-1.dll']
    if fs.exists(toolchain_bin / dll)
      fs.copyfile(toolchain_bin / dll, dll)
    endif
  endforeach
endif
