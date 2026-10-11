#pragma once
#if defined(_WIN32) && defined(LIBAI_CPP_SHARED)
#ifdef LIBAI_CPP_BUILD
#define LIBAI_CPP_API __declspec(dllexport)
#else
#define LIBAI_CPP_API __declspec(dllimport)
#endif
#else
#define LIBAI_CPP_API
#endif
