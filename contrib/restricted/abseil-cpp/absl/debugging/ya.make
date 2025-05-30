# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(20240722.1)

PEERDIR(
    contrib/restricted/abseil-cpp/absl/base
    library/cpp/sanitizer/include
)

ADDINCL(
    GLOBAL contrib/restricted/abseil-cpp
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    failure_signal_handler.cc
    internal/address_is_readable.cc
    internal/decode_rust_punycode.cc
    internal/demangle.cc
    internal/demangle_rust.cc
    internal/elf_mem_image.cc
    internal/examine_stack.cc
    internal/utf8_for_code_point.cc
    internal/vdso_support.cc
    leak_check.cc
    stacktrace.cc
    symbolize.cc
)

END()
