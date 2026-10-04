#pragma once

#ifndef WIN32
    #error "Error, WIN32 not defined"
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT	0xa00
#endif
#ifdef UNICODE
  #undef UNICODE
#endif

#define NOMINMAX

#include <winsock2.h>
#include <windows.h>

#include <string>

namespace dcn::native
{
    /**
     * @brief Appends `arg` to a CreateProcess command line, quoted so the child's argv
     * holds it unchanged.
     */
    void appendQuotedArgument(std::string & command_line, const std::string & arg);
}
