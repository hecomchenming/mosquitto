/* ============================================================
 * Control compile time options.
 * ============================================================
 *
 * Compile time options have moved to config.mk.
 */


/* ============================================================
 * Compatibility defines
 *
 * Generally for Windows native support.
 * ============================================================ */
#if defined(_MSC_VER) && _MSC_VER < 1900
#  define snprintf sprintf_s
#endif

#ifdef WIN32
#  ifndef strcasecmp
#    define strcasecmp strcmpi
#  endif
#define strtok_r strtok_s
#define strerror_r(e, b, l) strerror_s(b, l, e)
#endif


#define uthash_malloc(sz) _mosquitto_malloc(sz)
#define uthash_free(ptr,sz) _mosquitto_free(ptr)

#ifndef EPROTO
#  define EPROTO ECONNABORTED
#endif

#ifndef VERSION
# define VERSION "1.4.8"
#endif

#ifndef TIMESTAMP
# define TIMESTAMP "20160408"
#endif

#define MAX_CONNECT 1024000
#define MAX_EVENT 65536


//#ifndef WITH_BROKER
//#  define WITH_BROKER
//#endif

//#ifndef WITH_TLS
//# define WITH_TLS
//#endif

