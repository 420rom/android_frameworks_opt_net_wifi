/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wifi_system/wifi.h"
#define LOG_TAG "WifiHW"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cutils/log.h>
#include <cutils/memory.h>
#include <cutils/misc.h>
#include <cutils/properties.h>
#include <private/android_filesystem_config.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#ifdef LIBWPA_CLIENT_EXISTS
#include <libwpa_client/wpa_ctrl.h>
#else
#define WPA_EVENT_TERMINATING "CTRL-EVENT-TERMINATING "
struct wpa_ctrl {};
void wpa_ctrl_cleanup(void) {}
struct wpa_ctrl* wpa_ctrl_open(const char* ctrl_path) {
  return NULL;
}
void wpa_ctrl_close(struct wpa_ctrl* ctrl) {}
int wpa_ctrl_request(struct wpa_ctrl* ctrl, const char* cmd, size_t cmd_len,
                     char* reply, size_t* reply_len,
                     void (*msg_cb)(char* msg, size_t len)) {
  return 0;
}
int wpa_ctrl_attach(struct wpa_ctrl* ctrl) { return 0; }
int wpa_ctrl_detach(struct wpa_ctrl* ctrl) { return 0; }
int wpa_ctrl_recv(struct wpa_ctrl* ctrl, char* reply, size_t* reply_len) {
  return 0;
}
int wpa_ctrl_get_fd(struct wpa_ctrl* ctrl) { return 0; }
#endif  // defined LIBWPA_CLIENT_EXISTS

namespace android {
namespace wifi_system {
namespace {

/* socket pair used to exit from a blocking read */
int exit_sockets[2];
struct wpa_ctrl* ctrl_conn;
struct wpa_ctrl* monitor_conn;

static char primary_iface[PROPERTY_VALUE_MAX];
// TODO: use new ANDROID_SOCKET mechanism, once support for multiple
// sockets is in

#define WIFI_TEST_INTERFACE "sta"

#define WIFI_DRIVER_LOADER_DELAY 1000000

const char IFACE_DIR[] = "/data/system/wpa_supplicant";
const char SUPPLICANT_SERVICE_NAME[] = "wpa_supplicant";
const char SUPPLICANT_INIT_PROPERTY[] = "init.svc.wpa_supplicant";
const char SUPP_CONFIG_TEMPLATE[] = "/system/etc/wifi/wpa_supplicant.conf";
const char SUPP_CONFIG_FILE[] = "/data/misc/wifi/wpa_supplicant.conf";
const char P2P_CONFIG_FILE[] = "/data/misc/wifi/p2p_supplicant.conf";

const char IFNAME[] = "IFNAME=";
#define IFNAMELEN (sizeof(IFNAME) - 1)
const char WPA_EVENT_IGNORE[] = "CTRL-EVENT-IGNORE ";

unsigned char dummy_key[21] = {0x02, 0x11, 0xbe, 0x33, 0x43, 0x35, 0x68,
                               0x47, 0x84, 0x99, 0xa9, 0x2b, 0x1c, 0xd3,
                               0xee, 0xff, 0xf1, 0xe2, 0xf3, 0xf4, 0xf5};

void wifi_close_sockets() {
  if (ctrl_conn != NULL) {
    wpa_ctrl_close(ctrl_conn);
    ctrl_conn = NULL;
  }

  if (monitor_conn != NULL) {
    wpa_ctrl_close(monitor_conn);
    monitor_conn = NULL;
  }

  if (exit_sockets[0] >= 0) {
    close(exit_sockets[0]);
    exit_sockets[0] = -1;
  }

  if (exit_sockets[1] >= 0) {
    close(exit_sockets[1]);
    exit_sockets[1] = -1;
  }
}

int ensure_config_file_exists(const char* config_file) {
  char buf[2048];
  int srcfd, destfd;
  int nread;
  int ret;

  ret = access(config_file, R_OK | W_OK);
  if ((ret == 0) || (errno == EACCES)) {
    if ((ret != 0) &&
        (chmod(config_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0)) {
      ALOGE("Cannot set RW to \"%s\": %s", config_file, strerror(errno));
      return -1;
    }
    return 0;
  } else if (errno != ENOENT) {
    ALOGE("Cannot access \"%s\": %s", config_file, strerror(errno));
    return -1;
  }

  srcfd = TEMP_FAILURE_RETRY(open(SUPP_CONFIG_TEMPLATE, O_RDONLY));
  if (srcfd < 0) {
    ALOGE("Cannot open \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
    return -1;
  }

  destfd = TEMP_FAILURE_RETRY(open(config_file, O_CREAT | O_RDWR, 0660));
  if (destfd < 0) {
    close(srcfd);
    ALOGE("Cannot create \"%s\": %s", config_file, strerror(errno));
    return -1;
  }

  while ((nread = TEMP_FAILURE_RETRY(read(srcfd, buf, sizeof(buf)))) != 0) {
    if (nread < 0) {
      ALOGE("Error reading \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
      close(srcfd);
      close(destfd);
      unlink(config_file);
      return -1;
    }
    TEMP_FAILURE_RETRY(write(destfd, buf, nread));
  }

  close(destfd);
  close(srcfd);

  /* chmod is needed because open() didn't set permisions properly */
  if (chmod(config_file, 0660) < 0) {
    ALOGE("Error changing permissions of %s to 0660: %s", config_file,
          strerror(errno));
    unlink(config_file);
    return -1;
  }

  if (chown(config_file, AID_SYSTEM, AID_WIFI) < 0) {
    ALOGE("Error changing group ownership of %s to %d: %s", config_file,
          AID_WIFI, strerror(errno));
    unlink(config_file);
    return -1;
  }
  return 0;
}

}  // namespace

const char kWiFiEntropyFile[] = "/data/misc/wifi/entropy.bin";

int wifi_start_supplicant() {
  char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
  int count = 200; /* wait at most 20 seconds for completion */
  const prop_info* pi;
  unsigned serial = 0;

  /* Check whether already running */
  if (property_get(SUPPLICANT_INIT_PROPERTY, supp_status, NULL) &&
      strcmp(supp_status, "running") == 0) {
    return 0;
  }

  /* Before starting the daemon, make sure its config file exists */
  if (ensure_config_file_exists(SUPP_CONFIG_FILE) < 0) {
    ALOGE("Wi-Fi will not be enabled");
    return -1;
  }

  /*
   * Some devices have another configuration file for the p2p interface.
   * However, not all devices have this, and we'll let it slide if it
   * is missing.  For devices that do expect this file to exist,
   * supplicant will refuse to start and emit a good error message.
   * No need to check for it here.
   */
  (void)ensure_config_file_exists(P2P_CONFIG_FILE);

  if (ensure_entropy_file_exists() < 0) {
    ALOGE("Wi-Fi entropy file was not created");
  }

  /* Clear out any stale socket files that might be left over. */
  wpa_ctrl_cleanup();

  /* Reset sockets used for exiting from hung state */
  exit_sockets[0] = exit_sockets[1] = -1;

  /*
   * Get a reference to the status property, so we can distinguish
   * the case where it goes stopped => running => stopped (i.e.,
   * it start up, but fails right away) from the case in which
   * it starts in the stopped state and never manages to start
   * running at all.
   */
  pi = __system_property_find(SUPPLICANT_INIT_PROPERTY);
  if (pi != NULL) {
    serial = __system_property_serial(pi);
  }
  property_get("wifi.interface", primary_iface, WIFI_TEST_INTERFACE);

  property_set("ctl.start", SUPPLICANT_SERVICE_NAME);
  sched_yield();

  while (count-- > 0) {
    if (pi == NULL) {
      pi = __system_property_find(SUPPLICANT_INIT_PROPERTY);
    }
    if (pi != NULL) {
      /*
       * property serial updated means that init process is scheduled
       * after we sched_yield, further property status checking is based on this
       */
      if (__system_property_serial(pi) != serial) {
        __system_property_read(pi, NULL, supp_status);
        if (strcmp(supp_status, "running") == 0) {
          return 0;
        } else if (strcmp(supp_status, "stopped") == 0) {
          return -1;
        }
      }
    }
    usleep(100000);
  }
  return -1;
}

int wifi_stop_supplicant() {
  char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
  int count = 50; /* wait at most 5 seconds for completion */

  /* Check whether supplicant already stopped */
  if (property_get(SUPPLICANT_INIT_PROPERTY, supp_status, NULL) &&
      strcmp(supp_status, "stopped") == 0) {
    return 0;
  }

  property_set("ctl.stop", SUPPLICANT_SERVICE_NAME);
  sched_yield();

  while (count-- > 0) {
    if (property_get(SUPPLICANT_INIT_PROPERTY, supp_status, NULL)) {
      if (strcmp(supp_status, "stopped") == 0) return 0;
    }
    usleep(100000);
  }
  ALOGE("Failed to stop supplicant");
  return -1;
}

namespace {

int wifi_connect_on_socket_path(const char* path) {
  char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

  /* Make sure supplicant is running */
  if (!property_get(SUPPLICANT_INIT_PROPERTY, supp_status, NULL) ||
      strcmp(supp_status, "running") != 0) {
    ALOGE("Supplicant not running, cannot connect");
    return -1;
  }

  ctrl_conn = wpa_ctrl_open(path);
  if (ctrl_conn == NULL) {
    ALOGE("Unable to open connection to supplicant on \"%s\": %s", path,
          strerror(errno));
    return -1;
  }
  monitor_conn = wpa_ctrl_open(path);
  if (monitor_conn == NULL) {
    wpa_ctrl_close(ctrl_conn);
    ctrl_conn = NULL;
    return -1;
  }
  if (wpa_ctrl_attach(monitor_conn) != 0) {
    wpa_ctrl_close(monitor_conn);
    wpa_ctrl_close(ctrl_conn);
    ctrl_conn = monitor_conn = NULL;
    return -1;
  }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, exit_sockets) == -1) {
    wpa_ctrl_close(monitor_conn);
    wpa_ctrl_close(ctrl_conn);
    ctrl_conn = monitor_conn = NULL;
    return -1;
  }

  return 0;
}

int wifi_send_command(const char* cmd, char* reply, size_t* reply_len) {
  int ret;
  if (ctrl_conn == NULL) {
    ALOGV("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
    return -1;
  }
  ret = wpa_ctrl_request(ctrl_conn, cmd, strlen(cmd), reply, reply_len, NULL);
  if (ret == -2) {
    ALOGD("'%s' command timed out.\n", cmd);
    /* unblocks the monitor receive socket for termination */
    TEMP_FAILURE_RETRY(write(exit_sockets[0], "T", 1));
    return -2;
  } else if (ret < 0 || strncmp(reply, "FAIL", 4) == 0) {
    return -1;
  }
  if (strncmp(cmd, "PING", 4) == 0) {
    reply[*reply_len] = '\0';
  }
  return 0;
}

int wifi_supplicant_connection_active() {
  char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

  if (property_get(SUPPLICANT_INIT_PROPERTY, supp_status, NULL)) {
    if (strcmp(supp_status, "stopped") == 0) return -1;
  }

  return 0;
}

int wifi_ctrl_recv(char* reply, size_t* reply_len) {
  int res;
  int ctrlfd = wpa_ctrl_get_fd(monitor_conn);
  struct pollfd rfds[2];

  memset(rfds, 0, 2 * sizeof(struct pollfd));
  rfds[0].fd = ctrlfd;
  rfds[0].events |= POLLIN;
  rfds[1].fd = exit_sockets[1];
  rfds[1].events |= POLLIN;
  do {
    res = TEMP_FAILURE_RETRY(poll(rfds, 2, 30000));
    if (res < 0) {
      ALOGE("Error poll = %d", res);
      return res;
    } else if (res == 0) {
      /* timed out, check if supplicant is active
       * or not ..
       */
      res = wifi_supplicant_connection_active();
      if (res < 0) return -2;
    }
  } while (res == 0);

  if (rfds[0].revents & POLLIN) {
    return wpa_ctrl_recv(monitor_conn, reply, reply_len);
  }

  /* it is not rfds[0], then it must be rfts[1] (i.e. the exit socket)
   * or we timed out. In either case, this call has failed ..
   */
  return -2;
}

int wifi_wait_on_socket(char* buf, size_t buflen) {
  size_t nread = buflen - 1;
  int result;
  char* match;
  char* match2;

  if (monitor_conn == NULL) {
    return snprintf(buf, buflen, "IFNAME=%s %s - connection closed",
                    primary_iface, WPA_EVENT_TERMINATING);
  }

  result = wifi_ctrl_recv(buf, &nread);

  /* Terminate reception on exit socket */
  if (result == -2) {
    return snprintf(buf, buflen, "IFNAME=%s %s - connection closed",
                    primary_iface, WPA_EVENT_TERMINATING);
  }

  if (result < 0) {
    ALOGD("wifi_ctrl_recv failed: %s\n", strerror(errno));
    return snprintf(buf, buflen, "IFNAME=%s %s - recv error", primary_iface,
                    WPA_EVENT_TERMINATING);
  }
  buf[nread] = '\0';
  /* Check for EOF on the socket */
  if (result == 0 && nread == 0) {
    /* Fabricate an event to pass up */
    ALOGD("Received EOF on supplicant socket\n");
    return snprintf(buf, buflen, "IFNAME=%s %s - signal 0 received",
                    primary_iface, WPA_EVENT_TERMINATING);
  }
  /*
   * Events strings are in the format
   *
   *     IFNAME=iface <N>CTRL-EVENT-XXX
   *        or
   *     <N>CTRL-EVENT-XXX
   *
   * where N is the message level in numerical form (0=VERBOSE, 1=DEBUG,
   * etc.) and XXX is the event name. The level information is not useful
   * to us, so strip it off.
   */

  if (strncmp(buf, IFNAME, IFNAMELEN) == 0) {
    match = strchr(buf, ' ');
    if (match != NULL) {
      if (match[1] == '<') {
        match2 = strchr(match + 2, '>');
        if (match2 != NULL) {
          nread -= (match2 - match);
          memmove(match + 1, match2 + 1, nread - (match - buf) + 1);
        }
      }
    } else {
      return snprintf(buf, buflen, "%s", WPA_EVENT_IGNORE);
    }
  } else if (buf[0] == '<') {
    match = strchr(buf, '>');
    if (match != NULL) {
      nread -= (match + 1 - buf);
      memmove(buf, match + 1, nread + 1);
      ALOGV("supplicant generated event without interface - %s\n", buf);
    }
  } else {
    /* let the event go as is! */
    ALOGW(
        "supplicant generated event without interface and without message "
        "level - %s\n",
        buf);
  }

  return nread;
}

}  // namespace

/* Establishes the control and monitor socket connections on the interface */
int wifi_connect_to_supplicant() {
  static char path[PATH_MAX];

  if (access(IFACE_DIR, F_OK) == 0) {
    snprintf(path, sizeof(path), "%s/%s", IFACE_DIR, primary_iface);
  } else {
    snprintf(path, sizeof(path), "@android:wpa_%s", primary_iface);
  }
  return wifi_connect_on_socket_path(path);
}

void wifi_close_supplicant_connection() {
  char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
  int count =
      50; /* wait at most 5 seconds to ensure init has stopped stupplicant */

  wifi_close_sockets();

  while (count-- > 0) {
    if (property_get(SUPPLICANT_INIT_PROPERTY, supp_status, NULL)) {
      if (strcmp(supp_status, "stopped") == 0) return;
    }
    usleep(100000);
  }
}

int wifi_wait_for_event(char* buf, size_t buflen) {
  return wifi_wait_on_socket(buf, buflen);
}

int wifi_command(const char* command, char* reply, size_t* reply_len) {
  return wifi_send_command(command, reply, reply_len);
}

int ensure_entropy_file_exists() {
  int ret;
  int destfd;

  ret = access(kWiFiEntropyFile, R_OK | W_OK);
  if ((ret == 0) || (errno == EACCES)) {
    if ((ret != 0) &&
        (chmod(kWiFiEntropyFile, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0)) {
      ALOGE("Cannot set RW to \"%s\": %s", kWiFiEntropyFile, strerror(errno));
      return -1;
    }
    return 0;
  }
  destfd = TEMP_FAILURE_RETRY(open(kWiFiEntropyFile, O_CREAT | O_RDWR, 0660));
  if (destfd < 0) {
    ALOGE("Cannot create \"%s\": %s", kWiFiEntropyFile, strerror(errno));
    return -1;
  }

  if (TEMP_FAILURE_RETRY(write(destfd, dummy_key, sizeof(dummy_key))) !=
      sizeof(dummy_key)) {
    ALOGE("Error writing \"%s\": %s", kWiFiEntropyFile, strerror(errno));
    close(destfd);
    return -1;
  }
  close(destfd);

  /* chmod is needed because open() didn't set permisions properly */
  if (chmod(kWiFiEntropyFile, 0660) < 0) {
    ALOGE("Error changing permissions of %s to 0660: %s", kWiFiEntropyFile,
          strerror(errno));
    unlink(kWiFiEntropyFile);
    return -1;
  }

  if (chown(kWiFiEntropyFile, AID_SYSTEM, AID_WIFI) < 0) {
    ALOGE("Error changing group ownership of %s to %d: %s", kWiFiEntropyFile,
          AID_WIFI, strerror(errno));
    unlink(kWiFiEntropyFile);
    return -1;
  }
  return 0;
}

}  // namespace wifi_system
}  // namespace android
