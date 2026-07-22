# drogon v1.9.7's RequestStream.h uses std::exception_ptr without including
# <exception>. libstdc++ (Linux) pulls it in transitively; libc++ (macOS/
# Apple Clang) does not, so this fails outright there. Implemented as a CMake
# script (rather than sed) so it runs identically on GNU sed (Linux) and BSD
# sed (macOS) hosts, which disagree on \n handling in replacement text.
# PATCH_COMMAND runs with cwd set to the fetched source root (drogon-src),
# so this is relative to that, not to this script's own location.
set(_file "lib/inc/drogon/RequestStream.h")
file(READ "${_file}" _contents)
string(FIND "${_contents}" "#include <exception>" _already_patched)
if(_already_patched EQUAL -1)
    string(REPLACE "#include <functional>" "#include <exception>\n#include <functional>" _contents "${_contents}")
    file(WRITE "${_file}" "${_contents}")
endif()
