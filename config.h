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


//#ifndef WITH_BROKER
//#  define WITH_BROKER
//#endif

//#ifndef WITH_TLS
//# define WITH_TLS
//#endif

