/**
 * collectd - src/pcie_errors.c
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Kamil Wiatrowski <kamilx.wiatrowski@intel.com>
 **/

#include "collectd.h"

#include "common.h"
#include "utils_llist.h"
#include "utils_message_parser.h"

#include <fnmatch.h>
#include <linux/pci_regs.h>

#define PCIE_ERRORS_PLUGIN "pcie_errors"
#define PCIE_DEFAULT_PROCDIR "/proc/bus/pci"
#define PCIE_DEFAULT_SYSFSDIR "/sys/bus/pci"
#define PCIE_NAME_LEN 512
#define PCIE_BUFF_SIZE 1024

#define PCIE_ERROR "pcie_error"
#define PCIE_SEV_CE "correctable"
#define PCIE_SEV_FATAL "fatal"
#define PCIE_SEV_NOFATAL "non_fatal"

#define PCIE_DEV(x) (((x) >> 3) & 0x1f)
#define PCIE_FN(x) ((x) & 0x07)

#define PCIE_ECAP_OFFSET 0x100 /* ECAP always begin at offset 0x100 */

#define PCIE_LOG_PORT "root port"
#define PCIE_LOG_SEVERITY "severity"
#define PCIE_LOG_DEV "device"
#define PCIE_LOG_TYPE "error type"
#define PCIE_LOG_ID "id"

typedef struct pcie_config_s {
  _Bool use_sysfs;
  _Bool notif_masked;
  _Bool persistent;
  _Bool first_read;
  _Bool read_devices;
  _Bool read_log;
  _Bool default_patterns;
  char access_dir[PATH_MAX];
  char logfile[PATH_MAX];
  _Bool config_error;
} pcie_config_t;

typedef struct pcie_device_s {
  int fd;
  int domain;
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  int cap_exp;
  int ecap_aer;
  uint16_t device_status;
  uint32_t correctable_errors;
  uint32_t uncorrectable_errors;
} pcie_device_t;

typedef struct pcie_fops_s {
  int (*list_devices)(llist_t *dev_list);
  int (*open)(pcie_device_t *dev);
  void (*close)(pcie_device_t *dev);
  int (*read)(pcie_device_t *dev, void *buff, int size, int pos);
} pcie_fops_t;

typedef struct pcie_error_s {
  int mask;
  const char *desc;
} pcie_error_t;

typedef struct pcie_msg_parser_s {
  char name[PCIE_NAME_LEN];
  parser_job_data *job;
  message_pattern *patterns;
  size_t patterns_len;
} pcie_msg_parser_t;

static llist_t *pcie_dev_list;
static pcie_config_t pcie_config = {.access_dir = "",
                                    .logfile = "/var/log/syslog",
                                    .use_sysfs = 1,
                                    .read_devices = 1};
static pcie_fops_t pcie_fops;

/* Device Error Status */
static pcie_error_t pcie_base_errors[] = {
    {PCI_EXP_DEVSTA_CED, "Correctable Error"},
    {PCI_EXP_DEVSTA_NFED, "Non-Fatal Error"},
    {PCI_EXP_DEVSTA_FED, "Fatal Error"},
    {PCI_EXP_DEVSTA_URD, "Unsupported Request"}};
static const int pcie_base_errors_num = STATIC_ARRAY_SIZE(pcie_base_errors);

/* Uncorrectable Error Status */
static pcie_error_t pcie_aer_ues[] = {
    {PCI_ERR_UNC_DLP, "Data Link Protocol"},
    {PCI_ERR_UNC_SURPDN, "Surprise Down"},
    {PCI_ERR_UNC_POISON_TLP, "Poisoned TLP"},
    {PCI_ERR_UNC_FCP, "Flow Control Protocol"},
    {PCI_ERR_UNC_COMP_TIME, "Completion Timeout"},
    {PCI_ERR_UNC_COMP_ABORT, "Completer Abort"},
    {PCI_ERR_UNC_UNX_COMP, "Unexpected Completion"},
    {PCI_ERR_UNC_RX_OVER, "Receiver Overflow"},
    {PCI_ERR_UNC_MALF_TLP, "Malformed TLP"},
    {PCI_ERR_UNC_ECRC, "ECRC Error Status"},
    {PCI_ERR_UNC_UNSUP, "Unsupported Request"},
    {PCI_ERR_UNC_ACSV, "ACS Violation"},
    {PCI_ERR_UNC_INTN, "Internal"},
    {PCI_ERR_UNC_MCBTLP, "MC blocked TLP"},
    {PCI_ERR_UNC_ATOMEG, "Atomic egress blocked"},
    {PCI_ERR_UNC_TLPPRE, "TLP prefix blocked"}};
static const int pcie_aer_ues_num = STATIC_ARRAY_SIZE(pcie_aer_ues);

/* Correctable Error Status */
static pcie_error_t pcie_aer_ces[] = {
    {PCI_ERR_COR_RCVR, "Receiver Error Status"},
    {PCI_ERR_COR_BAD_TLP, "Bad TLP Status"},
    {PCI_ERR_COR_BAD_DLLP, "Bad DLLP Status"},
    {PCI_ERR_COR_REP_ROLL, "REPLAY_NUM Rollover"},
    {PCI_ERR_COR_REP_TIMER, "Replay Timer Timeout"},
    {PCI_ERR_COR_ADV_NFAT, "Advisory Non-Fatal"},
    {PCI_ERR_COR_INTERNAL, "Corrected Internal"},
    {PCI_ERR_COR_LOG_OVER, "Header Log Overflow"}};
static const int pcie_aer_ces_num = STATIC_ARRAY_SIZE(pcie_aer_ces);

static pcie_msg_parser_t *pcie_parsers = NULL;
static size_t pcie_parsers_len = 0;

/* Default patterns for AER errors in syslog */
static message_pattern pcie_default_patterns[] = {
    {.name = PCIE_LOG_PORT,
     .regex = "pcieport (.*): AER:",
     .submatch_idx = 1,
     .is_mandatory = 1},
    {.name = PCIE_LOG_DEV,
     .regex = " ([0-9a-fA-F:\\.]*): PCIe Bus Error",
     .submatch_idx = 1,
     .is_mandatory = 1},
    {.name = PCIE_LOG_SEVERITY,
     .regex = "severity=([^,]*)",
     .submatch_idx = 1,
     .is_mandatory = 1},
    {.name = PCIE_LOG_TYPE,
     .regex = "type=(.*),",
     .submatch_idx = 1,
     .is_mandatory = 0},
    {.name = PCIE_LOG_ID,
     .regex = ", id=(.*)",
     .submatch_idx = 1,
     .is_mandatory = 1}};

static int pcie_add_device(llist_t *list, int domain, uint8_t bus,
                           uint8_t device, uint8_t fn) {
  llentry_t *entry;
  pcie_device_t *dev = calloc(1, sizeof(*dev));
  if (dev == NULL) {
    ERROR(PCIE_ERRORS_PLUGIN ": Failed to allocate device");
    return -ENOMEM;
  }

  dev->domain = domain;
  dev->bus = bus;
  dev->device = device;
  dev->function = fn;
  dev->cap_exp = -1;
  dev->ecap_aer = -1;
  entry = llentry_create(NULL, dev);
  llist_append(list, entry);

  DEBUG(PCIE_ERRORS_PLUGIN ": pci device added to list: %04x:%02x:%02x.%d",
        domain, bus, device, fn);
  return 0;
}

static void pcie_clear_list(llist_t *list) {
  if (list == NULL)
    return;

  for (llentry_t *e = llist_head(list); e != NULL; e = e->next) {
    sfree(e->value);
  }

  llist_destroy(list);
}

static int pcie_list_devices_proc(llist_t *dev_list) {
  FILE *fd;
  char file_name[PCIE_NAME_LEN];
  char buf[PCIE_BUFF_SIZE];
  unsigned int i = 0;
  int ret = 0;

  if (dev_list == NULL)
    return -EINVAL;

  ssnprintf(file_name, sizeof(file_name), "%s/devices", pcie_config.access_dir);
  fd = fopen(file_name, "r");
  if (!fd) {
    char errbuf[PCIE_BUFF_SIZE];
    ERROR(PCIE_ERRORS_PLUGIN ": Cannot open file %s to get devices list: %s",
          file_name, sstrerror(errno, errbuf, sizeof(errbuf)));
    return -ENOENT;
  }

  while (fgets(buf, sizeof(buf), fd)) {
    unsigned int slot;
    uint8_t bus, dev, fn;

    if (sscanf(buf, "%x", &slot) != 1) {
      ERROR(PCIE_ERRORS_PLUGIN ": Failed to read line %u from %s", i + 1,
            file_name);
      continue;
    }

    bus = slot >> 8U;
    dev = PCIE_DEV(slot);
    fn = PCIE_FN(slot);
    ret = pcie_add_device(dev_list, 0, bus, dev, fn);
    if (ret)
      break;

    ++i;
  }

  fclose(fd);
  return ret;
}

static int pcie_list_devices_sysfs(llist_t *dev_list) {
  DIR *dir;
  struct dirent *item;
  char dir_name[PCIE_NAME_LEN];
  int ret = 0;

  if (dev_list == NULL)
    return -EINVAL;

  ssnprintf(dir_name, sizeof(dir_name), "%s/devices", pcie_config.access_dir);
  dir = opendir(dir_name);
  if (!dir) {
    char errbuf[PCIE_BUFF_SIZE];
    ERROR(PCIE_ERRORS_PLUGIN ": Cannot open dir %s to get devices list: %s",
          dir_name, sstrerror(errno, errbuf, sizeof(errbuf)));
    return -ENOENT;
  }

  while ((item = readdir(dir))) {
    int dom, bus, dev, fn;

    /* Omit special non-device entries */
    if (item->d_name[0] == '.')
      continue;

    if (sscanf(item->d_name, "%x:%x:%x.%d", &dom, &bus, &dev, &fn) != 4) {
      ERROR(PCIE_ERRORS_PLUGIN ": Failed to parse entry %s", item->d_name);
      continue;
    }

    ret = pcie_add_device(dev_list, dom, bus, dev, fn);
    if (ret)
      break;
  }

  closedir(dir);
  return ret;
}

static void pcie_close(pcie_device_t *dev) {
  if (close(dev->fd) == -1) {
    char errbuf[PCIE_BUFF_SIZE];
    ERROR(PCIE_ERRORS_PLUGIN ": Failed to close %04x:%02x:%02x.%d, fd=%d: %s",
          dev->domain, dev->bus, dev->device, dev->function, dev->fd,
          sstrerror(errno, errbuf, sizeof(errbuf)));
  }

  dev->fd = -1;
}

static int pcie_open(pcie_device_t *dev, const char *name) {
  dev->fd = open(name, O_RDWR);
  if (dev->fd == -1) {
    char errbuf[PCIE_BUFF_SIZE];
    ERROR(PCIE_ERRORS_PLUGIN ": Failed to open file %s: %s", name,
          sstrerror(errno, errbuf, sizeof(errbuf)));
    return -ENOENT;
  }

  return 0;
}

static int pcie_open_proc(pcie_device_t *dev) {
  char file_name[PCIE_NAME_LEN];

  ssnprintf(file_name, sizeof(file_name), "%s/%02x/%02x.%d",
            pcie_config.access_dir, dev->bus, dev->device, dev->function);

  return pcie_open(dev, file_name);
}

static int pcie_open_sysfs(pcie_device_t *dev) {
  char file_name[PCIE_NAME_LEN];

  ssnprintf(file_name, sizeof(file_name), "%s/devices/%04x:%02x:%02x.%d/config",
            pcie_config.access_dir, dev->domain, dev->bus, dev->device,
            dev->function);

  return pcie_open(dev, file_name);
}

static int pcie_read(pcie_device_t *dev, void *buff, int size, int pos) {
  int len = pread(dev->fd, buff, size, pos);
  if (len == size)
    return 0;

  if (len == -1) {
    char errbuf[PCIE_BUFF_SIZE];
    ERROR(PCIE_ERRORS_PLUGIN ": Failed to read %04x:%02x:%02x.%d at pos %d: %s",
          dev->domain, dev->bus, dev->device, dev->function, pos,
          sstrerror(errno, errbuf, sizeof(errbuf)));
  } else {
    ERROR(PCIE_ERRORS_PLUGIN
          ": %04x:%02x:%02x.%d Read only %d bytes, should be %d",
          dev->domain, dev->bus, dev->device, dev->function, len, size);
  }
  return -1;
}

static uint8_t pcie_read8(pcie_device_t *dev, int pos) {
  uint8_t value;
  if (pcie_fops.read(dev, &value, 1, pos))
    return 0;
  return value;
}

static uint16_t pcie_read16(pcie_device_t *dev, int pos) {
  uint16_t value;
  if (pcie_fops.read(dev, &value, 2, pos))
    return 0;
  return value;
}

static uint32_t pcie_read32(pcie_device_t *dev, int pos) {
  uint32_t value;
  if (pcie_fops.read(dev, &value, 4, pos))
    return 0;
  return value;
}

static void pcie_do_dispatch_notification(notification_t *n, const char *type) {
  sstrncpy(n->host, hostname_g, sizeof(n->host));
  sstrncpy(n->type, type, sizeof(n->type));

  plugin_dispatch_notification(n);
  if (n->meta != NULL)
    plugin_notification_meta_free(n->meta);
}

static void pcie_dispatch_notification(pcie_device_t *dev, notification_t *n,
                                       const char *type,
                                       const char *type_instance) {
  ssnprintf(n->plugin_instance, sizeof(n->plugin_instance), "%04x:%02x:%02x.%d",
            dev->domain, dev->bus, dev->device, dev->function);
  sstrncpy(n->type_instance, type_instance, sizeof(n->type_instance));

  pcie_do_dispatch_notification(n, type);
}

/* Report errors found in AER Correctable Error Status register */
static void pcie_dispatch_correctable_errors(pcie_device_t *dev,
                                             uint32_t errors, uint32_t masked) {
  for (int i = 0; i < pcie_aer_ces_num; i++) {
    pcie_error_t *err = pcie_aer_ces + i;
    notification_t n = {.severity = NOTIF_WARNING,
                        .time = cdtime(),
                        .plugin = PCIE_ERRORS_PLUGIN,
                        .meta = NULL};

    /* If not specifically set by config option omit masked errors */
    if (!pcie_config.notif_masked && (err->mask & masked))
      continue;

    if (err->mask & errors) {
      /* Error already reported, notify only if persistent is set */
      if (!pcie_config.persistent && (err->mask & dev->correctable_errors))
        continue;

      DEBUG(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: %s set", dev->domain,
            dev->bus, dev->device, dev->function, err->desc);
      ssnprintf(n.message, sizeof(n.message), "Correctable Error set: %s",
                err->desc);
      pcie_dispatch_notification(dev, &n, PCIE_ERROR, PCIE_SEV_CE);

    } else if (err->mask & dev->correctable_errors) {
      DEBUG(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: %s cleared", dev->domain,
            dev->bus, dev->device, dev->function, err->desc);

      n.severity = NOTIF_OKAY;
      ssnprintf(n.message, sizeof(n.message), "Correctable Error cleared: %s",
                err->desc);
      pcie_dispatch_notification(dev, &n, PCIE_ERROR, PCIE_SEV_CE);
    }
  }
}

/* Report errors found in AER Uncorrectable Error Status register */
static void pcie_dispatch_uncorrectable_errors(pcie_device_t *dev,
                                               uint32_t errors, uint32_t masked,
                                               uint32_t severity) {
  for (int i = 0; i < pcie_aer_ues_num; i++) {
    pcie_error_t *err = pcie_aer_ues + i;
    const char *type_instance =
        (severity & err->mask) ? PCIE_SEV_FATAL : PCIE_SEV_NOFATAL;
    notification_t n = {
        .time = cdtime(), .plugin = PCIE_ERRORS_PLUGIN, .meta = NULL};

    /* If not specifically set by config option omit masked errors */
    if (!pcie_config.notif_masked && (err->mask & masked))
      continue;

    if (err->mask & errors) {
      /* Error already reported, notify only if persistent is set */
      if (!pcie_config.persistent && (err->mask & dev->uncorrectable_errors))
        continue;

      DEBUG(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: %s(%s) set", dev->domain,
            dev->bus, dev->device, dev->function, err->desc, type_instance);

      n.severity = (severity & err->mask) ? NOTIF_FAILURE : NOTIF_WARNING;
      ssnprintf(n.message, sizeof(n.message), "Uncorrectable(%s) Error set: %s",
                type_instance, err->desc);
      pcie_dispatch_notification(dev, &n, PCIE_ERROR, type_instance);

    } else if (err->mask & dev->uncorrectable_errors) {
      DEBUG(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: %s(%s) cleared",
            dev->domain, dev->bus, dev->device, dev->function, err->desc,
            type_instance);

      n.severity = NOTIF_OKAY;
      ssnprintf(n.message, sizeof(n.message),
                "Uncorrectable(%s) Error cleared: %s", type_instance,
                err->desc);
      pcie_dispatch_notification(dev, &n, PCIE_ERROR, type_instance);
    }
  }
}

/* Find offset of PCI Express Capability Structure
 * in PCI configuration space.
 * Returns offset, -1 if not found.
**/
static int pcie_find_cap_exp(pcie_device_t *dev) {
  int pos = pcie_read8(dev, PCI_CAPABILITY_LIST) & ~3;

  while (pos) {
    uint8_t id = pcie_read8(dev, pos + PCI_CAP_LIST_ID);

    if (id == 0xff)
      break;
    if (id == PCI_CAP_ID_EXP)
      return pos;

    pos = pcie_read8(dev, pos + PCI_CAP_LIST_NEXT) & ~3;
  }

  DEBUG(PCIE_ERRORS_PLUGIN ": Cannot find CAP EXP for %04x:%02x:%02x.%d",
        dev->domain, dev->bus, dev->device, dev->function);

  return -1;
}

/* Find offset of Advanced Error Reporting Capability.
 * Returns AER offset, -1 if not found.
**/
static int pcie_find_ecap_aer(pcie_device_t *dev) {
  int pos = PCIE_ECAP_OFFSET;
  uint32_t header = pcie_read32(dev, pos);
  int id = PCI_EXT_CAP_ID(header);
  int next = PCI_EXT_CAP_NEXT(header);

  if (!id && !next)
    return -1;

  if (id == PCI_EXT_CAP_ID_ERR)
    return pos;

  while (next) {
    if (next <= PCIE_ECAP_OFFSET)
      break;

    header = pcie_read32(dev, next);
    id = PCI_EXT_CAP_ID(header);

    if (id == PCI_EXT_CAP_ID_ERR)
      return next;

    next = PCI_EXT_CAP_NEXT(header);
  }

  return -1;
}

static void pcie_check_dev_status(pcie_device_t *dev, int pos) {
  /* Read Device Status register with mask for errors only */
  uint16_t new_status = pcie_read16(dev, pos + PCI_EXP_DEVSTA) & 0xf;

  /* Check if anything new should be reported */
  if (!(pcie_config.persistent && new_status) &&
      (new_status == dev->device_status))
    return;

  /* Report errors found in Device Status register */
  for (int i = 0; i < pcie_base_errors_num; i++) {
    pcie_error_t *err = pcie_base_errors + i;
    const char *type_instance = (err->mask == PCI_EXP_DEVSTA_FED)
                                    ? PCIE_SEV_FATAL
                                    : (err->mask == PCI_EXP_DEVSTA_CED)
                                          ? PCIE_SEV_CE
                                          : PCIE_SEV_NOFATAL;
    const int severity =
        (err->mask == PCI_EXP_DEVSTA_FED) ? NOTIF_FAILURE : NOTIF_WARNING;
    notification_t n = {.severity = severity,
                        .time = cdtime(),
                        .plugin = PCIE_ERRORS_PLUGIN,
                        .meta = NULL};

    if (err->mask & new_status) {
      /* Error already reported, notify only if persistent is set */
      if (!pcie_config.persistent && (err->mask & dev->device_status))
        continue;

      DEBUG(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: %s set", dev->domain,
            dev->bus, dev->device, dev->function, err->desc);
      ssnprintf(n.message, sizeof(n.message), "Device Status Error set: %s",
                err->desc);
      pcie_dispatch_notification(dev, &n, PCIE_ERROR, type_instance);

    } else if (err->mask & dev->device_status) {
      DEBUG(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: %s cleared", dev->domain,
            dev->bus, dev->device, dev->function, err->desc);
      n.severity = NOTIF_OKAY;
      ssnprintf(n.message, sizeof(n.message), "Device Status Error cleared: %s",
                err->desc);
      pcie_dispatch_notification(dev, &n, PCIE_ERROR, type_instance);
    }
  }

  dev->device_status = new_status;
}

static void pcie_check_aer(pcie_device_t *dev, int pos) {
  /* Check for AER uncorrectable errors */
  uint32_t errors = pcie_read32(dev, pos + PCI_ERR_UNCOR_STATUS);

  if ((pcie_config.persistent && errors) ||
      (errors != dev->uncorrectable_errors)) {
    uint32_t masked = pcie_read32(dev, pos + PCI_ERR_UNCOR_MASK);
    uint32_t severity = pcie_read32(dev, pos + PCI_ERR_UNCOR_SEVER);
    pcie_dispatch_uncorrectable_errors(dev, errors, masked, severity);
  }
  dev->uncorrectable_errors = errors;

  /* Check for AER correctable errors */
  errors = pcie_read32(dev, pos + PCI_ERR_COR_STATUS);
  if ((pcie_config.persistent && errors) ||
      (errors != dev->correctable_errors)) {
    uint32_t masked = pcie_read32(dev, pos + PCI_ERR_COR_MASK);
    pcie_dispatch_correctable_errors(dev, errors, masked);
  }
  dev->correctable_errors = errors;
}

static int pcie_process_devices(llist_t *devs) {
  int ret = 0;
  if (devs == NULL)
    return -1;

  for (llentry_t *e = llist_head(devs); e != NULL; e = e->next) {
    pcie_device_t *dev = e->value;

    if (pcie_fops.open(dev) == 0) {
      pcie_check_dev_status(dev, dev->cap_exp);
      if (dev->ecap_aer != -1)
        pcie_check_aer(dev, dev->ecap_aer);

      pcie_fops.close(dev);
    } else {
      notification_t n = {.severity = NOTIF_FAILURE,
                          .time = cdtime(),
                          .message = "Failed to read device status",
                          .plugin = PCIE_ERRORS_PLUGIN,
                          .meta = NULL};
      pcie_dispatch_notification(dev, &n, "", "");
      ret = -1;
    }
  }

  return ret;
}

/* This function is to be called during init to filter out not pcie devices */
static void pcie_preprocess_devices(llist_t *devs) {
  llentry_t *e_next;

  if (devs == NULL)
    return;

  for (llentry_t *e = llist_head(devs); e != NULL; e = e_next) {
    pcie_device_t *dev = e->value;
    _Bool del = 0;

    if (pcie_fops.open(dev) == 0) {
      uint16_t status = pcie_read16(dev, PCI_STATUS);
      if (status & PCI_STATUS_CAP_LIST)
        dev->cap_exp = pcie_find_cap_exp(dev);

      /* Every PCIe device must have Capability Structure */
      if (dev->cap_exp == -1) {
        DEBUG(PCIE_ERRORS_PLUGIN ": Not PCI Express device: %04x:%02x:%02x.%d",
              dev->domain, dev->bus, dev->device, dev->function);
        del = 1;
      } else {
        dev->ecap_aer = pcie_find_ecap_aer(dev);
        if (dev->ecap_aer == -1)
          INFO(PCIE_ERRORS_PLUGIN ": Device is not AER capable: %04x:%02x:%02x.%d",
                dev->domain, dev->bus, dev->device, dev->function);
      }

      pcie_fops.close(dev);
    } else {
      ERROR(PCIE_ERRORS_PLUGIN ": %04x:%02x:%02x.%d: failed to open",
            dev->domain, dev->bus, dev->device, dev->function);
      del = 1;
    }

    e_next = e->next;
    if (del) {
      sfree(dev);
      llist_remove(devs, e);
      llentry_destroy(e);
    }
  }
}

static void pcie_parse_msg(message *msg, unsigned int max_items) {
  notification_t n = {.severity = NOTIF_WARNING,
                      .time = cdtime(),
                      .plugin = PCIE_ERRORS_PLUGIN,
                      .meta = NULL};

  for (int i = 0; i < max_items; i++) {
    message_item *item = msg->message_items + i;
    if (!item->value[0])
      break;

    DEBUG(PCIE_ERRORS_PLUGIN "[%02d] %s:%s", i, item->name, item->value);

    if (strncmp(item->name, PCIE_LOG_SEVERITY, strlen(PCIE_LOG_SEVERITY)) ==
        0) {
      if (fnmatch("*[nN]on-[fF]atal*", item->value, 0) == 0) {
        sstrncpy(n.type_instance, PCIE_SEV_NOFATAL, sizeof(n.type_instance));
      } else if (fnmatch("*[fF]atal*", item->value, 0) == 0) {
        n.severity = NOTIF_FAILURE;
        sstrncpy(n.type_instance, PCIE_SEV_FATAL, sizeof(n.type_instance));
      } else {
        sstrncpy(n.type_instance, PCIE_SEV_CE, sizeof(n.type_instance));
      }
    } else if (strncmp(item->name, PCIE_LOG_DEV, strlen(PCIE_LOG_DEV)) == 0) {
      sstrncpy(n.plugin_instance, item->value, sizeof(n.plugin_instance));
    } else {
      if (plugin_notification_meta_add_string(&n, item->name, item->value))
        ERROR(PCIE_ERRORS_PLUGIN ": Failed to add notification meta data %s:%s",
              item->name, item->value);
    }
  }
  ssnprintf(n.message, sizeof(n.message), "AER %s error reported in log",
            n.type_instance);

  pcie_do_dispatch_notification(&n, PCIE_ERROR);
}

static int pcie_logfile_read(parser_job_data *job, const char *name) {
  message *messages_storage;
  unsigned int max_item_num;
  int msg_num =
      message_parser_read(job, &messages_storage, pcie_config.first_read);
  if (msg_num < 0) {
    notification_t n = {.severity = NOTIF_FAILURE,
                        .time = cdtime(),
                        .message = "Failed to read from log file",
                        .plugin = PCIE_ERRORS_PLUGIN,
                        .meta = NULL};
    pcie_do_dispatch_notification(&n, "");
    return -1;
  }

  max_item_num = STATIC_ARRAY_SIZE(messages_storage[0].message_items);

  DEBUG(PCIE_ERRORS_PLUGIN ": read %d messages, %s", msg_num, name);

  for (int i = 0; i < msg_num; i++) {
    message *msg = messages_storage + i;
    pcie_parse_msg(msg, max_item_num);
  }
  return 0;
}

static int pcie_plugin_read(__attribute__((unused)) user_data_t *ud) {
  int ret = 0;

  if (pcie_config.read_devices) {
    ret = pcie_process_devices(pcie_dev_list);
    if (ret < 0) {
      ERROR(PCIE_ERRORS_PLUGIN ": Failed to read devices state");
      return -1;
    }
  }

  if (!pcie_config.read_log)
    return 0;

  for (int i = 0; i < pcie_parsers_len; i++) {
    ret = pcie_logfile_read(pcie_parsers[i].job, pcie_parsers[i].name);
    if (ret < 0) {
      ERROR(PCIE_ERRORS_PLUGIN ": Failed to parse %s messages from %s",
            pcie_parsers[i].name, pcie_config.logfile);
      break;
    }
  }

  if (pcie_config.first_read)
    pcie_config.first_read = 0;

  return ret;
}

static void pcie_access_config(void) {
  /* Set functions for register access to
   * use proc or sysfs depending on config. */
  if (pcie_config.use_sysfs) {
    pcie_fops.list_devices = pcie_list_devices_sysfs;
    pcie_fops.open = pcie_open_sysfs;
    if (pcie_config.access_dir[0] == '\0')
      sstrncpy(pcie_config.access_dir, PCIE_DEFAULT_SYSFSDIR,
               sizeof(pcie_config.access_dir));
  } else {
    /* use proc */
    pcie_fops.list_devices = pcie_list_devices_proc;
    pcie_fops.open = pcie_open_proc;
    if (pcie_config.access_dir[0] == '\0')
      sstrncpy(pcie_config.access_dir, PCIE_DEFAULT_PROCDIR,
               sizeof(pcie_config.access_dir));
  }
  /* Common functions */
  pcie_fops.close = pcie_close;
  pcie_fops.read = pcie_read;
}

static int pcie_patterns_config(message_pattern *patterns,
                                oconfig_item_t *match_opt, int num) {
  for (int i = 0; i < num; i++) {
    if (strcasecmp("Match", match_opt[i].key) == 0) {
      /* set default submatch index to 1 since single submatch is the most
       * common use case */
      patterns[i].submatch_idx = 1;
      for (int j = 0; j < match_opt[i].children_num; j++) {
        int status = 0;
        oconfig_item_t *regex_opt = match_opt[i].children + j;
        if (strcasecmp("Name", regex_opt->key) == 0)
          status = cf_util_get_string(regex_opt, &(patterns[i].name));
        else if (strcasecmp("Regex", regex_opt->key) == 0)
          status = cf_util_get_string(regex_opt, &(patterns[i].regex));
        else if (strcasecmp("SubmatchIdx", regex_opt->key) == 0)
          status = cf_util_get_int(regex_opt, &(patterns[i].submatch_idx));
        else if (strcasecmp("Excluderegex", regex_opt->key) == 0)
          status = cf_util_get_string(regex_opt, &(patterns[i].excluderegex));
        else if (strcasecmp("IsMandatory", regex_opt->key) == 0)
          status = cf_util_get_boolean(regex_opt, &(patterns[i].is_mandatory));
        else {
          ERROR(PCIE_ERRORS_PLUGIN ": Invalid configuration option \"%s\".",
                regex_opt->key);
          return -1;
        }

        if (status) {
          ERROR(PCIE_ERRORS_PLUGIN ": Error setting regex option %s",
                regex_opt->key);
        }
      }
    } else {
      ERROR(PCIE_ERRORS_PLUGIN ": option \"%s\" is not allowed here.",
            match_opt[i].key);
      return -1;
    }
  }

  return 0;
}

static int pcie_plugin_config(oconfig_item_t *ci) {
  int parser_idx = 0;
  /* Get number of configured parsers in advance */
  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    if (strcasecmp("MsgPattern", child->key) == 0)
      pcie_parsers_len++;
  }

  for (int i = 0; i < ci->children_num; i++) {
    oconfig_item_t *child = ci->children + i;
    int status = 0;

    if (strcasecmp("Source", child->key) == 0) {
      if ((child->values_num != 1) ||
          (child->values[0].type != OCONFIG_TYPE_STRING)) {
        status = -1;
      } else if (strcasecmp("proc", child->values[0].value.string) == 0) {
        pcie_config.use_sysfs = 0;
      } else if (strcasecmp("sysfs", child->values[0].value.string) != 0) {
        pcie_config.use_sysfs = 0;
        pcie_config.read_devices = 0;
      }
    } else if (strcasecmp("AccessDir", child->key) == 0) {
      status = cf_util_get_string_buffer(child, pcie_config.access_dir,
                                         sizeof(pcie_config.access_dir));
    } else if (strcasecmp("ReportMasked", child->key) == 0) {
      status = cf_util_get_boolean(child, &pcie_config.notif_masked);
    } else if (strcasecmp("PersistentNotifications", child->key) == 0) {
      status = cf_util_get_boolean(child, &pcie_config.persistent);
    } else if (strcasecmp("LogFile", child->key) == 0) {
      status = cf_util_get_string_buffer(child, pcie_config.logfile,
                                         sizeof(pcie_config.logfile));
    } else if (strcasecmp("ReadLog", child->key) == 0) {
      status = cf_util_get_boolean(child, &pcie_config.read_log);
    } else if (strcasecmp("FirstFullRead", child->key) == 0) {
      status = cf_util_get_boolean(child, &pcie_config.first_read);
    } else if (strcasecmp("MsgPattern", child->key) == 0) {
      if (!pcie_parsers) {
        pcie_parsers = calloc(pcie_parsers_len, sizeof(*pcie_parsers));
        if (pcie_parsers == NULL) {
          ERROR(PCIE_ERRORS_PLUGIN ": Error allocating message parsers");
          pcie_config.config_error = 1;
          break;
        }
      }
      if (cf_util_get_string_buffer(child, pcie_parsers[parser_idx].name,
                                    sizeof(pcie_parsers[parser_idx].name))) {
        ERROR(PCIE_ERRORS_PLUGIN ": Invalid configuration parameter \"%s\".",
              child->key);
        pcie_config.config_error = 1;
        break;
      }
      pcie_parsers[parser_idx].patterns_len = child->children_num;
      pcie_parsers[parser_idx].patterns =
          calloc(pcie_parsers[parser_idx].patterns_len,
                 sizeof(*(pcie_parsers[parser_idx].patterns)));
      if (pcie_parsers[parser_idx].patterns == NULL) {
        ERROR(PCIE_ERRORS_PLUGIN ": Error allocating message_patterns");
        pcie_config.config_error = 1;
        break;
      }
      if (pcie_patterns_config(pcie_parsers[parser_idx].patterns,
                               child->children, child->children_num)) {
        ERROR(PCIE_ERRORS_PLUGIN ": Failed to parse patterns for \"%s\".",
              child->key);
        pcie_config.config_error = 1;
        break;
      }
      parser_idx++;
    } else {
      ERROR(PCIE_ERRORS_PLUGIN ": Invalid configuration option \"%s\".",
            child->key);
      pcie_config.config_error = 1;
      break;
    }

    if (status) {
      ERROR(PCIE_ERRORS_PLUGIN ": Invalid configuration parameter \"%s\".",
            child->key);
      pcie_config.config_error = 1;
      break;
    }
  }

  return 0;
}

static int pcie_shutdown(void) {
  pcie_clear_list(pcie_dev_list);
  pcie_dev_list = NULL;

  for (int i = 0; i < pcie_parsers_len; i++) {
    if (pcie_parsers[i].job != NULL)
      message_parser_cleanup(pcie_parsers[i].job);
    if (!pcie_config.default_patterns)
      sfree(pcie_parsers[i].patterns);
  }

  if (pcie_parsers)
    sfree(pcie_parsers);

  return 0;
}

static int pcie_init(void) {
  if (pcie_config.config_error) {
    ERROR(PCIE_ERRORS_PLUGIN
          ": Error in configuration, failed to init plugin.");
    return -1;
  }

  if (!pcie_config.read_devices && !pcie_config.read_log) {
    ERROR(PCIE_ERRORS_PLUGIN
          ": Plugin is not configured for any source of data.");
    return -1;
  }

  if (pcie_config.read_devices) {
    pcie_access_config();
    pcie_dev_list = llist_create();
    if (pcie_fops.list_devices(pcie_dev_list) != 0) {
      ERROR(PCIE_ERRORS_PLUGIN ": Failed to find devices.");
      pcie_shutdown();
      return -1;
    }
    pcie_preprocess_devices(pcie_dev_list);
    if (llist_size(pcie_dev_list) == 0) {
      /* No any PCI Express devices were found on the system */
      ERROR(PCIE_ERRORS_PLUGIN ": No PCIe devices found in %s",
      pcie_config.access_dir);
      pcie_shutdown();
      return -1;
    }
  }

  if (!pcie_config.read_log)
    return 0;

  if (pcie_parsers == NULL) {
    /* Set default patterns if no config is provided */
    INFO(PCIE_ERRORS_PLUGIN ": Using default message parser");
    pcie_config.default_patterns = 1;
    pcie_parsers = calloc(1, sizeof(*pcie_parsers));
    if (pcie_parsers == NULL) {
      ERROR(PCIE_ERRORS_PLUGIN ": Error allocating default message parser");
      pcie_shutdown();
      return -1;
    }
    pcie_parsers_len = 1;
    sstrncpy(pcie_parsers[0].name, "default", sizeof(pcie_parsers[0].name));
    pcie_parsers[0].patterns = pcie_default_patterns;
    pcie_parsers[0].patterns_len = STATIC_ARRAY_SIZE(pcie_default_patterns);
  }

  for (int i = 0; i < pcie_parsers_len; i++) {
    pcie_parsers[i].job = message_parser_init(
        pcie_config.logfile, 0, pcie_parsers[i].patterns_len - 1,
        pcie_parsers[i].patterns, pcie_parsers[i].patterns_len);
    if (pcie_parsers[i].job == NULL) {
      ERROR(PCIE_ERRORS_PLUGIN ": Failed to initialize %s parser.",
            pcie_parsers[i].name);
      pcie_shutdown();
      return -1;
    }
  }

  return 0;
}

void module_register(void) {
  plugin_register_init(PCIE_ERRORS_PLUGIN, pcie_init);
  plugin_register_complex_config(PCIE_ERRORS_PLUGIN, pcie_plugin_config);
  plugin_register_complex_read(NULL, PCIE_ERRORS_PLUGIN, pcie_plugin_read, 0,
                               NULL);
  plugin_register_shutdown(PCIE_ERRORS_PLUGIN, pcie_shutdown);
}
