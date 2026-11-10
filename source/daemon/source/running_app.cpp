/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Running BigApp query. Separate from daemon_utils.cpp so the standalone FPS
 * monitor can link the FPS sampler without the launcher.
 */

#include "daemon_ops.hpp"

#include <onion/log.h>

#include <atomic>
#include <string.h>

extern "C" {
  int sceSystemServiceGetAppIdOfRunningBigApp();
  int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
}

bool Get_Running_App_TID(std::string &title_id, int &BigAppid) {
  char tid[255];
  BigAppid = sceSystemServiceGetAppIdOfRunningBigApp();
  if (BigAppid < 0)
    return false;

  (void)memset(tid, 0, sizeof tid);
  /* Polled every sample tick by two threads: log only rc changes, and only
   * the first few so a flapping rc cannot flood the log. */
  static std::atomic<int> s_last_rc{0};
  static std::atomic<int> s_logs{0};
  const int rc = sceSystemServiceGetAppTitleId(BigAppid, &tid[0]);
  const int prev_rc = s_last_rc.exchange(rc);
  const bool report = rc != prev_rc && s_logs.fetch_add(1) < 4;
  if (rc != 0) {
    if (report)
      LOG_WARN("running-app: sceSystemServiceGetAppTitleId(app=%d) failed "
               "rc=0x%x",
               BigAppid, static_cast<unsigned>(rc));
    return false;
  }
  if (report)
    LOG_INFO("running-app: title id query recovered app=%d tid=%s", BigAppid,
             tid);

  title_id = std::string(tid);
  return true;
}
