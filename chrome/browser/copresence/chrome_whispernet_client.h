// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_COPRESENCE_CHROME_WHISPERNET_CLIENT_H_
#define CHROME_BROWSER_COPRESENCE_CHROME_WHISPERNET_CLIENT_H_

#include <string>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/scoped_vector.h"
#include "components/copresence/public/copresence_constants.h"
#include "components/copresence/public/whispernet_client.h"

namespace content {
class BrowserContext;
}

namespace extensions {

struct Event;
class EventRouter;

namespace api {
namespace copresence_private {
struct AudioParameters;
}
}

}  // namespace extensions

namespace media {
class AudioBusRefCounted;
}

// This class is responsible for communication with our ledger_proxy extension
// that talks to the whispernet audio library.
class ChromeWhispernetClient final : public copresence::WhispernetClient {
 public:
  // The browser context needs to outlive this class.
  explicit ChromeWhispernetClient(content::BrowserContext* browser_context);
  ~ChromeWhispernetClient() override;

  // WhispernetClient overrides:
  void Initialize(const copresence::SuccessCallback& init_callback) override;
  void Shutdown() override;
  void EncodeToken(const std::string& token_str,
                   copresence::AudioType type) override;
  void DecodeSamples(copresence::AudioType type,
                     const std::string& samples,
                     const size_t token_length[2]) override;
  void DetectBroadcast() override;
  void RegisterTokensCallback(
      const copresence::TokensCallback& tokens_callback) override;
  void RegisterSamplesCallback(
      const copresence::SamplesCallback& samples_callback) override;
  void RegisterDetectBroadcastCallback(
      const copresence::SuccessCallback& db_callback) override;

  copresence::TokensCallback GetTokensCallback() override;
  copresence::SamplesCallback GetSamplesCallback() override;
  copresence::SuccessCallback GetDetectBroadcastCallback() override;
  copresence::SuccessCallback GetInitializedCallback() override;

  static const char kWhispernetProxyExtensionId[];

 private:
  // Fire an event to configure whispernet with the given audio parameters.
  void AudioConfiguration(const copresence::config::AudioParamData& params);

  void SendEventIfLoaded(scoped_ptr<extensions::Event> event);

  // This gets called twice; once when the proxy extension loads, the second
  // time when we have initialized the proxy extension's encoder and decoder.
  void OnExtensionLoaded(bool success);

  content::BrowserContext* const browser_context_;
  extensions::EventRouter* const event_router_;

  copresence::SuccessCallback extension_loaded_callback_;
  copresence::SuccessCallback init_callback_;

  copresence::TokensCallback tokens_callback_;
  copresence::SamplesCallback samples_callback_;
  copresence::SuccessCallback db_callback_;

  ScopedVector<extensions::Event> queued_events_;
  bool extension_loaded_;

  DISALLOW_COPY_AND_ASSIGN(ChromeWhispernetClient);
};

#endif  // CHROME_BROWSER_COPRESENCE_CHROME_WHISPERNET_CLIENT_H_
