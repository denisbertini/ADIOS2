#ifndef _CM_CONFIG_H
#define _CM_CONFIG_H

#define EVPATH_VERSION "5.1.3"

/* config.h.in.  Generated from configure.ac by autoheader.  */

/* Define to 1 if you have the <enet/enet.h> header file. */
/* #undef HAVE_ENET_ENET_H */

/* Define to desired port range Low:High */
#define EVPATH_DEFAULT_PORT_RANGE "ANY"

/* Define to 1 if the enet transport is built */
/* #undef EVPATH_HAS_ENET */

/* Define to 1 if the ZPL ENET transport is built */
/* #undef EVPATH_HAS_ZPL_ENET */

/* Define to 1 if the udt4 transport is built */
/* #undef EVPATH_HAS_UDT4 */

/* Define to 1 if the IB transport is built */
/* #undef EVPATH_HAS_IB */

/* Define to 1 if the libfabric transport is built */
/* #undef EVPATH_HAS_LIBFABRIC */

/* Place where evpath transport modules are installed */
#define EVPATH_MODULE_INSTALL_DIR "/usr/local/lib"

/* Place where evpath tests are to be installed */
/* #undef EVPATH_TEST_INSTALL_DIR */

/* Place where the ssh binary can be found */
#define SSH_PATH "/usr/bin/ssh"

/* Define to 1 if you have the <dlfcn.h> header file. */
/* #undef HAVE_DLFCN_H */

/* Define if fd_set has element fds_bits */
/* #undef HAVE_FDS_BITS */

/* Define to 1 if you have the `getdomainname' function. */
#define HAVE_GETDOMAINNAME

/* Define to 1 if you have the `getloadavg' function. */
#define HAVE_GETLOADAVG

/* Define to 1 if you have the `gettimeofday' function. */
#define HAVE_GETTIMEOFDAY

/* Define to 1 if you have the `pthread' library (-lpthread). */
/* #undef HAVE_LIBPTHREAD */

/* Uses the Mac OS X style conventions for sysctl */
/* #undef HAVE_MAC_SYSCTL */

/* Define if system uses LINUX-style sysctl */
/* #undef HAVE_LINUX_SYSCTL */

/* Define to 1 if you have the <malloc.h> header file. */
#define HAVE_MALLOC_H

/* Define to 1 if you have the <math.h> header file. */
/* #undef HAVE_MATH_H */

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H

/* Define to 1 if you have the <netdb.h> header file. */
#define HAVE_NETDB_H

/* Define to 1 if you have the <netinet/in.h> header file. */
#define HAVE_NETINET_IN_H

/* Define to 1 if you have the <arpa/inet.h> header file. */
#define HAVE_ARPA_INET_H

/* Define to 1 if you have the <stdarg.h> header file. */
/* #undef HAVE_STDARG_H */

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H

/* Define to 1 if you have the <strings.h> header file. */
/* #undef HAVE_STRINGS_H */

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H

/* Define to 1 if you have the `sysconf' function. */
#define HAVE_SYSCONF

/* Define to 1 if you have the `sysinfo' function. */
#define HAVE_SYSINFO

/* Define to 1 if you have the <sys/epoll.h> header file. */
#define HAVE_SYS_EPOLL_H

/* Define to 1 if you have the <cod.h> header file. */
#define HAVE_COD_H

/* Define to 1 if you have the <sys/select.h> header file. */
#define HAVE_SYS_SELECT_H

/* Define to 1 if you have the <sys/sockio.h> header file. */
/* #undef HAVE_SYS_SOCKIO_H */

/* Define to 1 if you have the <sys/stat.h> header file. */
/* #undef HAVE_SYS_STAT_H */

/* Define to 1 if you have the <sys/sysctl.h> header file. */
/* #undef HAVE_SYS_SYSCTL_H */

/* Define to 1 if you have the <sys/times.h> header file. */
#define HAVE_SYS_TIMES_H

/* Define to 1 if you have the <sys/time.h> header file. */
#define HAVE_SYS_TIME_H

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H

/* Define to 1 if you have the <sys/uio.h> header file. */
#define HAVE_SYS_UIO_H

/* Define to 1 if you have the <sys/un.h> header file. */
#define HAVE_SYS_UN_H

/* Define to 1 if you have the `uname' function. */
#define HAVE_UNAME

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H

/* Define to 1 if you have the <windows.h> header file. */
/* #undef HAVE_WINDOWS_H */

/* Define to 1 if you have the <winsock2.h> header file. */
/* #undef HAVE_WINSOCK2_H */

/* The size of `int', as computed by sizeof. */
#define SIZEOF_INT 4

/* The size of `long', as computed by sizeof. */
#define SIZEOF_LONG 8

/* Define to 1 if you have the `writev' function. */
#define HAVE_WRITEV

/* Define to 1 if you have the `getifaddrs' function. */
#define HAVE_GETIFADDRS

/* Define to 1 if you have the `clock_gettime' function. */
#define HAVE_CLOCK_GETTIME

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS

/* Define this if Pthreads should be used for running tests */
#define USE_PTHREADS

/* Version number of package */
/* #undef VERSION */

/* Define so that glibc/gnulib argp.h does not typedef error_t. */
/* #undef __error_t_defined */

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define to a type to use for `error_t' if it is not otherwise available. */
/* #undef error_t */

/* Define to `int' if <sys/types.h> does not define. */
/* #undef pid_t */

/* Define to `int' if <sys/types.h> does not define. */
/* #undef pid_t */

/* Define to the shared module suffix in use */
#define CMAKE_SHARED_MODULE_SUFFIX ".so"

/* Set to 1 to build without dynamic linking  */
#define NO_DYNAMIC_LINKING 1

/* Define to 1 if the NNTI transport is built */
/* #undef EVPATH_HAS_NNTI */

/* Set to 1 if NVML libraries and include file are found */
/* #undef NVML_FOUND */

/* Set to 1 if CM should default to CMSelfFormats */
#define CM_SELF_FORMATS 1


/* Define if byteorder is bigendian */
/* #undef WORDS_BIGENDIAN */

#define CM_DEFAULT_TRANSPORT "sockets"

#define CM_LIBRARY_PREFIX "adios2_"

#define IPCONFIG_ENVVAR_PREFIX "ADIOS2_"

#endif
