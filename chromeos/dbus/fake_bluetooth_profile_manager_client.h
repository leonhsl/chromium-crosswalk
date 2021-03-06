// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_DBUS_FAKE_BLUETOOTH_PROFILE_MANAGER_CLIENT_H_
#define CHROMEOS_DBUS_FAKE_BLUETOOTH_PROFILE_MANAGER_CLIENT_H_

#include <map>
#include <string>

#include "base/bind.h"
#include "base/callback.h"
#include "base/observer_list.h"
#include "chromeos/chromeos_export.h"
#include "chromeos/dbus/bluetooth_profile_manager_client.h"
#include "dbus/object_path.h"
#include "dbus/property.h"

namespace chromeos {

class FakeBluetoothProfileServiceProvider;

// FakeBluetoothProfileManagerClient simulates the behavior of the Bluetooth
// Daemon's profile manager object and is used both in test cases in place of a
// mock and on the Linux desktop.
class CHROMEOS_EXPORT FakeBluetoothProfileManagerClient
    : public BluetoothProfileManagerClient {
 public:
  FakeBluetoothProfileManagerClient();
  ~FakeBluetoothProfileManagerClient() override;

  // BluetoothProfileManagerClient overrides
  void Init(dbus::Bus* bus) override;
  void RegisterProfile(const dbus::ObjectPath& profile_path,
                       const std::string& uuid,
                       const Options& options,
                       const base::Closure& callback,
                       const ErrorCallback& error_callback) override;
  void UnregisterProfile(const dbus::ObjectPath& profile_path,
                         const base::Closure& callback,
                         const ErrorCallback& error_callback) override;

  // Register, unregister and retrieve pointers to profile server providers.
  void RegisterProfileServiceProvider(
      FakeBluetoothProfileServiceProvider* service_provider);
  void UnregisterProfileServiceProvider(
      FakeBluetoothProfileServiceProvider* service_provider);
  FakeBluetoothProfileServiceProvider* GetProfileServiceProvider(
      const std::string& uuid);

  // UUIDs recognised for testing.
  static const char kL2capUuid[];
  static const char kRfcommUuid[];

 private:
  // Map of a D-Bus object path to the FakeBluetoothProfileServiceProvider
  // registered for it; maintained by RegisterProfileServiceProvider() and
  // UnregisterProfileServiceProvicer() called by the constructor and
  // destructor of FakeBluetoothProfileServiceProvider.
  typedef std::map<dbus::ObjectPath, FakeBluetoothProfileServiceProvider*>
      ServiceProviderMap;
  ServiceProviderMap service_provider_map_;

  // Map of Profile UUID to the D-Bus object path of the service provider
  // in |service_provider_map_|. Maintained by RegisterProfile() and
  // UnregisterProfile() in response to BluetoothProfile methods.
  typedef std::map<std::string, dbus::ObjectPath> ProfileMap;
  ProfileMap profile_map_;
};

}  // namespace chromeos

#endif  // CHROMEOS_DBUS_FAKE_BLUETOOTH_PROFILE_MANAGER_CLIENT_H_
