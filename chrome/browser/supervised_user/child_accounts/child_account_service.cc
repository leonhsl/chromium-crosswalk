// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/supervised_user/child_accounts/child_account_service.h"

#include "base/callback.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/message_loop/message_loop.h"
#include "base/path_service.h"
#include "base/prefs/pref_service.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/profile_oauth2_token_service_factory.h"
#include "chrome/browser/signin/signin_manager_factory.h"
#include "chrome/browser/supervised_user/supervised_user_constants.h"
#include "chrome/browser/supervised_user/supervised_user_service.h"
#include "chrome/browser/supervised_user/supervised_user_service_factory.h"
#include "chrome/browser/supervised_user/supervised_user_settings_service.h"
#include "chrome/browser/supervised_user/supervised_user_settings_service_factory.h"
#include "chrome/browser/sync/profile_sync_service.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "components/signin/core/browser/profile_oauth2_token_service.h"
#include "components/signin/core/browser/signin_manager.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "components/user_manager/user_manager.h"
#endif

const char kIsChildAccountServiceFlagName[] = "uca";

ChildAccountService::ChildAccountService(Profile* profile)
    : profile_(profile), active_(false), weak_ptr_factory_(this) {}

ChildAccountService::~ChildAccountService() {}

void ChildAccountService::Init() {
  SigninManagerFactory::GetForProfile(profile_)->AddObserver(this);
  SupervisedUserServiceFactory::GetForProfile(profile_)->SetDelegate(this);

  PropagateChildStatusToUser(IsChildAccount());

  // If we're already signed in, fetch the flag again just to be sure.
  // (Previously, the browser might have been closed before we got the flag.
  // This also handles the graduation use case in a basic way.)
  std::string account_id = SigninManagerFactory::GetForProfile(profile_)
      ->GetAuthenticatedAccountId();
  if (!account_id.empty())
    StartFetchingServiceFlags(account_id);
}

void ChildAccountService::Shutdown() {
  CancelFetchingServiceFlags();
  SupervisedUserService* service =
      SupervisedUserServiceFactory::GetForProfile(profile_);
  service->SetDelegate(NULL);
  DCHECK(!active_);
  SigninManagerFactory::GetForProfile(profile_)->RemoveObserver(this);
}

bool ChildAccountService::IsChildAccount() const {
  return profile_->GetPrefs()->GetString(prefs::kSupervisedUserId) ==
             supervised_users::kChildAccountSUID;
}

bool ChildAccountService::SetActive(bool active) {
  if (!IsChildAccount() && !active_)
    return false;
  if (active_ == active)
    return true;
  active_ = active;

  if (active_) {
    // In contrast to local SUs, child account SUs must sign in.
    scoped_ptr<base::Value> allow_signin(new base::FundamentalValue(true));
    SupervisedUserSettingsService* settings_service =
        SupervisedUserSettingsServiceFactory::GetForProfile(profile_);
    settings_service->SetLocalSetting(supervised_users::kSigninAllowed,
                                      allow_signin.Pass());
#if !defined(OS_CHROMEOS)
    // This is also used by user policies (UserPolicySigninService), but since
    // child accounts can not also be Dasher accounts, there shouldn't be any
    // problems.
    SigninManagerFactory::GetForProfile(profile_)->ProhibitSignout(true);
#endif

    // TODO(treib): Maybe only fetch the parents on the first start, and then
    // refresh occasionally (like once every 24h)? That's what
    // GAIAInfoUpdateService does.
    family_fetcher_.reset(new FamilyInfoFetcher(
        this,
        SigninManagerFactory::GetForProfile(profile_)
            ->GetAuthenticatedAccountId(),
        ProfileOAuth2TokenServiceFactory::GetForProfile(profile_),
        profile_->GetRequestContext()));
    family_fetcher_->StartGetFamilyMembers();

    // Set the permission request API URL and scope, unless they have been
    // explicitly specified on the command line.
    CommandLine* command_line = CommandLine::ForCurrentProcess();
    if (!command_line->HasSwitch(switches::kPermissionRequestApiUrl)) {
      command_line->AppendSwitchASCII(
          switches::kPermissionRequestApiUrl,
          "https://www.googleapis.com/"
              "kidsmanagement/v1/people/me/permissionRequests");
    }
    if (!command_line->HasSwitch(switches::kPermissionRequestApiScope)) {
      command_line->AppendSwitchASCII(
          switches::kPermissionRequestApiScope,
          "https://www.googleapis.com/auth/kid.permission");
    }
  } else {
    SupervisedUserSettingsService* settings_service =
        SupervisedUserSettingsServiceFactory::GetForProfile(profile_);
    settings_service->SetLocalSetting(supervised_users::kSigninAllowed,
                                      scoped_ptr<base::Value>());
#if !defined(OS_CHROMEOS)
    SigninManagerFactory::GetForProfile(profile_)->ProhibitSignout(false);
#endif

    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserCustodianName);
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserCustodianEmail);
  }

  // Trigger a sync reconfig to enable/disable the right SU data types.
  // The logic to do this lives in the SupervisedUserSyncDataTypeController.
  ProfileSyncService* sync_service =
      ProfileSyncServiceFactory::GetForProfile(profile_);
  if (sync_service->HasSyncSetupCompleted())
    sync_service->ReconfigureDatatypeManager();

  return true;
}

base::FilePath ChildAccountService::GetBlacklistPath() const {
  if (!active_)
    return base::FilePath();
  base::FilePath blacklist_path;
  PathService::Get(chrome::DIR_USER_DATA, &blacklist_path);
  blacklist_path = blacklist_path.AppendASCII("su-blacklist.bin");
  return blacklist_path;
}

GURL ChildAccountService::GetBlacklistURL() const {
  if (!active_)
    return GURL();
  return GURL("https://www.gstatic.com/chrome/supervised_user/"
              "blacklist-20141001-1k.bin");
}

std::string ChildAccountService::GetSafeSitesCx() const {
  if (!active_)
    return std::string();
  return "017993620680222980993%3A1wdumejvx5i";
}

void ChildAccountService::GoogleSigninSucceeded(const std::string& account_id,
                                                const std::string& username,
                                                const std::string& password) {
  DCHECK(!account_id.empty());

  StartFetchingServiceFlags(account_id);
}

void ChildAccountService::GoogleSignedOut(const std::string& account_id,
                                          const std::string& username) {
  DCHECK(!IsChildAccount());
  CancelFetchingServiceFlags();
}

void ChildAccountService::OnGetFamilyMembersSuccess(
    const std::vector<FamilyInfoFetcher::FamilyMember>& members) {
  bool hoh_found = false;
  bool parent_found = false;
  for (const FamilyInfoFetcher::FamilyMember& member : members) {
    if (member.role == FamilyInfoFetcher::HEAD_OF_HOUSEHOLD) {
      hoh_found = true;
      profile_->GetPrefs()->SetString(prefs::kSupervisedUserCustodianName,
                                      member.display_name);
      profile_->GetPrefs()->SetString(prefs::kSupervisedUserCustodianEmail,
                                      member.email);
      profile_->GetPrefs()->SetString(prefs::kSupervisedUserCustodianProfileURL,
                                      member.profile_url);
      profile_->GetPrefs()->SetString(
          prefs::kSupervisedUserCustodianProfileImageURL,
          member.profile_image_url);
    } else if (member.role == FamilyInfoFetcher::PARENT) {
      parent_found = true;
      profile_->GetPrefs()->SetString(prefs::kSupervisedUserSecondCustodianName,
                                      member.display_name);
      profile_->GetPrefs()->SetString(
          prefs::kSupervisedUserSecondCustodianEmail,
          member.email);
      profile_->GetPrefs()->SetString(
          prefs::kSupervisedUserSecondCustodianProfileURL,
          member.profile_url);
      profile_->GetPrefs()->SetString(
          prefs::kSupervisedUserSecondCustodianProfileImageURL,
          member.profile_image_url);
    }
    if (hoh_found && parent_found)
      break;
  }
  if (!hoh_found) {
    DLOG(WARNING) << "GetFamilyMembers didn't return a HOH?!";
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserCustodianName);
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserCustodianEmail);
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserCustodianProfileURL);
    profile_->GetPrefs()->ClearPref(
        prefs::kSupervisedUserCustodianProfileImageURL);
  }
  if (!parent_found) {
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserSecondCustodianName);
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserSecondCustodianEmail);
    profile_->GetPrefs()->ClearPref(
        prefs::kSupervisedUserSecondCustodianProfileURL);
    profile_->GetPrefs()->ClearPref(
        prefs::kSupervisedUserSecondCustodianProfileImageURL);
  }
}

void ChildAccountService::OnFailure(FamilyInfoFetcher::ErrorCode error) {
  DLOG(WARNING) << "GetFamilyMembers failed with code " << error;
  // TODO(treib): Retry after a while?
}

void ChildAccountService::StartFetchingServiceFlags(
    const std::string& account_id) {
  account_id_ = account_id;
  flag_fetcher_.reset(new AccountServiceFlagFetcher(
      account_id_,
      ProfileOAuth2TokenServiceFactory::GetForProfile(profile_),
      profile_->GetRequestContext(),
      base::Bind(&ChildAccountService::OnFlagsFetched,
                 weak_ptr_factory_.GetWeakPtr())));
}

void ChildAccountService::CancelFetchingServiceFlags() {
  flag_fetcher_.reset();
  account_id_.clear();
}

void ChildAccountService::OnFlagsFetched(
    AccountServiceFlagFetcher::ResultCode result,
    const std::vector<std::string>& flags) {
  // If we've been signed out again (or signed in to a different account),
  // ignore the fetched flags.
  const std::string& new_account_id =
      SigninManagerFactory::GetForProfile(profile_)
          ->GetAuthenticatedAccountId();
  if (account_id_.empty() || account_id_ != new_account_id)
    return;

  // In case of an error, retry after a while.
  if (result != AccountServiceFlagFetcher::SUCCESS) {
    DLOG(WARNING) << "AccountServiceFlagFetcher returned error code " << result;
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&ChildAccountService::StartFetchingServiceFlags,
                   weak_ptr_factory_.GetWeakPtr(),
                   account_id_),
        base::TimeDelta::FromSeconds(10));
    return;
  }

  account_id_.clear();

  bool is_child_account =
      std::find(flags.begin(), flags.end(),
                kIsChildAccountServiceFlagName) != flags.end();
  SetIsChildAccount(is_child_account);
}

void ChildAccountService::SetIsChildAccount(bool is_child_account) {
  if (IsChildAccount() == is_child_account)
    return;

  if (is_child_account) {
    profile_->GetPrefs()->SetString(prefs::kSupervisedUserId,
                                    supervised_users::kChildAccountSUID);
  } else {
    profile_->GetPrefs()->ClearPref(prefs::kSupervisedUserId);
  }
  PropagateChildStatusToUser(is_child_account);
}

void ChildAccountService::PropagateChildStatusToUser(bool is_child) {
#if defined(OS_CHROMEOS)
  // TODO(merkulova,treib): Figure out why this causes tests to fail.
//  user_manager::User* user =
//      chromeos::ProfileHelper::Get()->GetUserByProfile(profile_);
//  if (user) {
//    user_manager::UserManager::Get()->ChangeUserSupervisedStatus(
//        user, is_child);
//  } else {
//    LOG(WARNING) <<
//        "User instance wasn't found while setting child account flag.";
//  }
#endif
}
