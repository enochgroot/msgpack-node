{
  "targets": [
    {
      "target_name": "msgpackBinding",
      "sources": [ "src/msgpack.cc" ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")"
      ],
      "defines": [
        "MSGPACK_EMBED_STACK_SIZE=512"
      ],
      "dependencies": [
        "deps/msgpack/msgpack.gyp:libmsgpack"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "cflags_cc": [ "-fexceptions", "-Wall" ],
      "cflags": [ "-Wall" ],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.15",
        "OTHER_CFLAGS": [ "-fexceptions", "-Wall" ]
      },
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1
        }
      }
    }
  ]
}
