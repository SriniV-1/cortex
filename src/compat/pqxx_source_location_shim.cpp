// Shim for libpqxx ABI mismatch on Ubuntu 24.04.
//
// Ubuntu's libpqxx 7.8.1 was compiled without C++20 source_location support,
// but its headers check __cpp_lib_source_location at compile time and declare
// constructors that accept std::source_location when compiling with C++20.
// This creates a linker error: the headers promise a symbol the library
// doesn't provide.
//
// This file provides the missing constructors by delegating to the single-arg
// versions that DO exist in the precompiled library.

#if !defined(__APPLE__) && __has_include(<source_location>)
#include <source_location>
#if __cpp_lib_source_location >= 201907L

#include <pqxx/except>

namespace pqxx
{

conversion_overrun::conversion_overrun(
    std::string const& msg, std::source_location)
    : conversion_overrun{msg}
{}

} // namespace pqxx

#endif // __cpp_lib_source_location
#endif // !__APPLE__
