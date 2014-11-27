// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/cronet/url_request_context_config.h"

#include "net/url_request/url_request_context_builder.h"

namespace cronet {

#define DEFINE_CONTEXT_CONFIG(x) const char REQUEST_CONTEXT_CONFIG_##x[] = #x;
#include "components/cronet/url_request_context_config_list.h"
#undef DEFINE_CONTEXT_CONFIG

#if !defined(DISABLE_QUIC_SUPPORT)
URLRequestContextConfig::QuicHint::QuicHint() {
}

URLRequestContextConfig::QuicHint::~QuicHint() {
}

// static
void URLRequestContextConfig::QuicHint::RegisterJSONConverter(
    base::JSONValueConverter<URLRequestContextConfig::QuicHint>* converter) {
  converter->RegisterStringField(REQUEST_CONTEXT_CONFIG_QUIC_HINT_HOST,
                                 &URLRequestContextConfig::QuicHint::host);
  converter->RegisterIntField(
      REQUEST_CONTEXT_CONFIG_QUIC_HINT_PORT,
      &URLRequestContextConfig::QuicHint::port);
  converter->RegisterIntField(
      REQUEST_CONTEXT_CONFIG_QUIC_HINT_ALT_PORT,
      &URLRequestContextConfig::QuicHint::alternate_port);
}
#endif  // !defined(DISABLE_QUIC_SUPPORT)

URLRequestContextConfig::URLRequestContextConfig() {
}

URLRequestContextConfig::~URLRequestContextConfig() {
}

void URLRequestContextConfig::ConfigureURLRequestContextBuilder(
    net::URLRequestContextBuilder* context_builder) {
  std::string config_cache;
  if (http_cache != REQUEST_CONTEXT_CONFIG_HTTP_CACHE_DISABLED) {
    net::URLRequestContextBuilder::HttpCacheParams cache_params;
    if (http_cache == REQUEST_CONTEXT_CONFIG_HTTP_CACHE_DISK &&
        !storage_path.empty()) {
      cache_params.type = net::URLRequestContextBuilder::HttpCacheParams::DISK;
      cache_params.path = base::FilePath(storage_path);
    } else {
      cache_params.type =
          net::URLRequestContextBuilder::HttpCacheParams::IN_MEMORY;
    }
    cache_params.max_size = http_cache_max_size;
    context_builder->EnableHttpCache(cache_params);
  } else {
    context_builder->DisableHttpCache();
  }

#if !defined(DISABLE_QUIC_SUPPORT)
  context_builder->SetSpdyAndQuicEnabled(enable_spdy, enable_quic);
#else
  context_builder->SetSpdyAndQuicEnabled(enable_spdy, false);
#endif
  // TODO(mef): Use |config| to set cookies.
}

// static
void URLRequestContextConfig::RegisterJSONConverter(
    base::JSONValueConverter<URLRequestContextConfig>* converter) {
#if !defined(DISABLE_QUIC_SUPPORT)
  converter->RegisterBoolField(REQUEST_CONTEXT_CONFIG_ENABLE_QUIC,
                               &URLRequestContextConfig::enable_quic);
#endif
  converter->RegisterBoolField(REQUEST_CONTEXT_CONFIG_ENABLE_SPDY,
                               &URLRequestContextConfig::enable_spdy);
  converter->RegisterStringField(REQUEST_CONTEXT_CONFIG_HTTP_CACHE,
                                 &URLRequestContextConfig::http_cache);
  converter->RegisterIntField(REQUEST_CONTEXT_CONFIG_HTTP_CACHE_MAX_SIZE,
                              &URLRequestContextConfig::http_cache_max_size);
  converter->RegisterStringField(REQUEST_CONTEXT_CONFIG_STORAGE_PATH,
                                 &URLRequestContextConfig::storage_path);
  converter->RegisterRepeatedMessage(REQUEST_CONTEXT_CONFIG_QUIC_HINTS,
                                     &URLRequestContextConfig::quic_hints);
}

}  // namespace cronet
