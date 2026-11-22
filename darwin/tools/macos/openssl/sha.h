/* the three one-shot digests pacstrip uses, from macOS' CommonCrypto (no OpenSSL headers on this Mac). */
#include <CommonCrypto/CommonDigest.h>
#define SHA_DIGEST_LENGTH       CC_SHA1_DIGEST_LENGTH
#define SHA256_DIGEST_LENGTH    CC_SHA256_DIGEST_LENGTH
#define SHA384_DIGEST_LENGTH    CC_SHA384_DIGEST_LENGTH
#define SHA1(d, n, md)          CC_SHA1((d), (CC_LONG)(n), (md))
#define SHA256(d, n, md)        CC_SHA256((d), (CC_LONG)(n), (md))
#define SHA384(d, n, md)        CC_SHA384((d), (CC_LONG)(n), (md))
