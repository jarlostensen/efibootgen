#pragma once

#include <iostream>
#include <fstream>
#include <functional>

#if __cplusplus < 201703L
#error "C++ version < 17 not supported"
#endif

#ifdef _WIN32
#define xstricmp(a,b) _stricmp(a,b)
#else
#include <cstring>
#include <strings.h>
#define xstricmp(a,b) strcasecmp(a,b)
#endif

