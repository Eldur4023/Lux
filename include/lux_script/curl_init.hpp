#pragma once
#include <curl/curl.h>

#include <cstdlib>

// curl_easy_init() that also trusts the CA bundle the Android shell builds (src/android.cpp points
// SSL_CERT_FILE at it: the system's hashed cert directory does not resolve with the statically linked
// OpenSSL 3). Plain curl_easy_init() everywhere else.
inline CURL* lux_curl_init() {
    CURL* c = curl_easy_init();
#ifdef __ANDROID__
    if (c)
        if (const char* ca = std::getenv("SSL_CERT_FILE")) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
#endif
    return c;
}
