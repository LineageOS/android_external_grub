#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/env.h>
#include <grub/file.h>
#include <grub/mm.h>
#include <grub/dl.h>
#include <grub/i18n.h>
#include <grub/types.h>

GRUB_MOD_LICENSE("GPLv3+");

#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024

#define VENDOR_BOOT_MAGIC "VNDRBOOT"
#define VENDOR_BOOT_MAGIC_SIZE 8
#define VENDOR_BOOT_ARGS_SIZE 2048
#define VENDOR_BOOT_NAME_SIZE 16

#define VENDOR_RAMDISK_TYPE_NONE 0
#define VENDOR_RAMDISK_TYPE_PLATFORM 1
#define VENDOR_RAMDISK_TYPE_RECOVERY 2
#define VENDOR_RAMDISK_TYPE_DLKM 3
#define VENDOR_RAMDISK_NAME_SIZE 32
#define VENDOR_RAMDISK_TABLE_ENTRY_BOARD_ID_SIZE 16

struct boot_img_hdr_v0 {
  grub_uint8_t magic[BOOT_MAGIC_SIZE];

  grub_uint32_t kernel_size;
  grub_uint32_t kernel_addr;

  grub_uint32_t ramdisk_size;
  grub_uint32_t ramdisk_addr;

  grub_uint32_t second_size;
  grub_uint32_t second_addr;

  grub_uint32_t tags_addr;
  grub_uint32_t page_size;

  grub_uint32_t header_version;
  grub_uint32_t os_version;

  grub_uint8_t name[BOOT_NAME_SIZE];
  grub_uint8_t cmdline[BOOT_ARGS_SIZE];

  grub_uint32_t id[8];

  grub_uint8_t extra_cmdline[BOOT_EXTRA_ARGS_SIZE];
} __attribute__((packed));

struct boot_img_hdr_v1 {
  struct boot_img_hdr_v0 v0;
  grub_uint32_t recovery_dtbo_size;
  grub_uint64_t recovery_dtbo_offset;
  grub_uint32_t header_size;
} __attribute__((packed));

struct boot_img_hdr_v2 {
  struct boot_img_hdr_v1 v1;
  grub_uint32_t dtb_size;
  grub_uint64_t dtb_addr;
} __attribute__((packed));

struct boot_img_hdr_v3 {
  grub_uint8_t magic[BOOT_MAGIC_SIZE];

  grub_uint32_t kernel_size;
  grub_uint32_t ramdisk_size;

  grub_uint32_t os_version;

  grub_uint32_t header_size;

  grub_uint32_t reserved[4];

  grub_uint32_t header_version;

  grub_uint8_t cmdline[BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE];
} __attribute__((packed));

struct boot_img_hdr_v4 {
  struct boot_img_hdr_v3 v3;
  grub_uint32_t signature_size;
} __attribute__((packed));

union boot_img_hdr_union {
  struct boot_img_hdr_v0 v0;
  struct boot_img_hdr_v1 v1;
  struct boot_img_hdr_v2 v2;
  struct boot_img_hdr_v3 v3;
  struct boot_img_hdr_v4 v4;
} __attribute__((packed));

enum android_boot_img_component {
  COMPONENT_KERNEL = 0,
  COMPONENT_RAMDISK,
  COMPONENT_SECOND,
  COMPONENT_RECOVERY_DTBO,
  COMPONENT_DTB,
  COMPONENT_INFO,
  COMPONENT_COUNT
};

const char* android_boot_img_component_filename[COMPONENT_COUNT] = {
  [COMPONENT_KERNEL] = "kernel", [COMPONENT_RAMDISK] = "ramdisk.img",
  [COMPONENT_SECOND] = "second", [COMPONENT_RECOVERY_DTBO] = "recovery_dtbo",
  [COMPONENT_DTB] = "dtb", [COMPONENT_INFO] = "info.cfg"
};

struct android_boot_file_data {
  grub_disk_t disk;
  enum android_boot_img_component component;
  char* buf;
};

/* COMPONENT_INFO content */
#define COMPONENT_INFO_FMT_BOOT_V0_V1_V2 \
  "set android_boot_img_name=\"%s\"\n" \
  "set android_boot_img_cmdline=\"%s\"\n"
static char* get_info_content_boot_v0_v1_v2(union boot_img_hdr_union hdr) {
  return grub_xasprintf(COMPONENT_INFO_FMT_BOOT_V0_V1_V2, hdr.v0.name, hdr.v0.cmdline);
}

/* Directory listing */
#define CHECK_SIZE_AND_REPORT(in_size, component)                             \
  grub_memset(&info, 0, sizeof(info));                                        \
  if (hdr.in_size)                                                            \
    hook(android_boot_img_component_filename[component], &info, hook_data);
#define REPORT_COMPONENT_INFO() \
  grub_memset(&info, 0, sizeof(info)); \
  hook(android_boot_img_component_filename[COMPONENT_INFO], &info, hook_data);
static grub_err_t
android_boot_dir(grub_device_t device,
                 const char* path __attribute__((unused)),
                 grub_fs_dir_hook_t hook, void* hook_data) {
  struct grub_dirhook_info info;
  union boot_img_hdr_union hdr;

  if (grub_disk_read(device->disk, 0, 0, sizeof(hdr), &hdr)) {
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_BAD_FS, "Failed to load boot image header from disk");
    return grub_errno;
  }

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0) {
    switch (hdr.v0.header_version) {
      case 2:
        CHECK_SIZE_AND_REPORT(v2.dtb_size, COMPONENT_DTB)
        [[fallthrough]];
      case 1:
        CHECK_SIZE_AND_REPORT(v1.recovery_dtbo_size, COMPONENT_RECOVERY_DTBO)
        [[fallthrough]];
      case 0:
        CHECK_SIZE_AND_REPORT(v0.kernel_size, COMPONENT_KERNEL)
        CHECK_SIZE_AND_REPORT(v0.ramdisk_size, COMPONENT_RAMDISK)
        CHECK_SIZE_AND_REPORT(v0.second_size, COMPONENT_SECOND)
        REPORT_COMPONENT_INFO()
        break;
      case 4:
        [[fallthrough]];
      case 3:
        CHECK_SIZE_AND_REPORT(v3.kernel_size, COMPONENT_KERNEL)
        CHECK_SIZE_AND_REPORT(v3.ramdisk_size, COMPONENT_RAMDISK)
        break;
      default:
        grub_printf("%s: %d\n", __func__, __LINE__);
        grub_error(GRUB_ERR_BAD_FS, "Unhandled Android boot header version");
        return grub_errno;
    }
  } else if (grub_memcmp(hdr.v0.magic, VENDOR_BOOT_MAGIC,
                         VENDOR_BOOT_MAGIC_SIZE)
             == 0) {
    // TODO
  } else {
    grub_printf("%s: %d - %s\n", __func__, __LINE__, hdr.v0.magic);
    grub_error(GRUB_ERR_BAD_FS, "Not Android boot image");
    return grub_errno;
  }

  return GRUB_ERR_NONE;
}
#undef CHECK_SIZE_AND_REPORT
#undef REPORT_COMPONENT_INFO

/* Open a file */
#define CHECK_SIZE_AND_REPORT(in_size, component)                             \
  if (i == component && hdr.in_size) {                                        \
    file->size = hdr.in_size;                                                 \
    goto report;                                                              \
  }
#define REPORT_COMPONENT_INFO(hdr_ver) \
  if (i == COMPONENT_INFO) { \
    data->buf = get_info_content_##hdr_ver(hdr); \
    if (data->buf) { \
      file->size = grub_strlen(data->buf); \
      goto report; \
    } \
  }
static grub_err_t
android_boot_open(grub_file_t file, const char* path) {
  struct android_boot_file_data* data;
  unsigned int i;
  union boot_img_hdr_union hdr;

  if (grub_disk_read(file->device->disk, 0, 0, sizeof(hdr), &hdr)) {
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_BAD_FS, "Failed to load boot image header from disk");
    return grub_errno;
  }

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0
      || grub_memcmp(hdr.v0.magic, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_SIZE)
             == 0) {
    for (i = 0; i < COMPONENT_COUNT; i++) {
      if (!grub_strcmp(path + 1, android_boot_img_component_filename[i])) {
        goto proceed;
      }
    }
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_BAD_FS, "Path %s is unsupported", path);
    return grub_errno;
  } else {
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_BAD_FS, "Not Android boot image");
    return grub_errno;
  }

proceed:
  data = grub_zalloc(sizeof(*data));
  if (!data) {
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_BAD_FS, "Unable to allocate memory");
    return grub_errno;
  }

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0) {
    switch (hdr.v0.header_version) {
      case 2:
        CHECK_SIZE_AND_REPORT(v2.dtb_size, COMPONENT_DTB)
        [[fallthrough]];
      case 1:
        CHECK_SIZE_AND_REPORT(v1.recovery_dtbo_size, COMPONENT_RECOVERY_DTBO)
        [[fallthrough]];
      case 0:
        CHECK_SIZE_AND_REPORT(v0.kernel_size, COMPONENT_KERNEL)
        CHECK_SIZE_AND_REPORT(v0.ramdisk_size, COMPONENT_RAMDISK)
        CHECK_SIZE_AND_REPORT(v0.second_size, COMPONENT_SECOND)
        REPORT_COMPONENT_INFO(boot_v0_v1_v2)
        break;
      case 4:
        [[fallthrough]];
      case 3:
        CHECK_SIZE_AND_REPORT(v3.kernel_size, COMPONENT_KERNEL)
        CHECK_SIZE_AND_REPORT(v3.ramdisk_size, COMPONENT_RAMDISK)
        break;
      default:
        grub_printf("%s: %d\n", __func__, __LINE__);
        grub_error(GRUB_ERR_BAD_FS, "Unhandled Android boot header version");
        return grub_errno;
    }
  } else if (grub_memcmp(hdr.v0.magic, VENDOR_BOOT_MAGIC,
                         VENDOR_BOOT_MAGIC_SIZE)
             == 0) {
    // TODO
  }

  grub_printf("%s: %d\n", __func__, __LINE__);
  grub_error(GRUB_ERR_FILE_NOT_FOUND, "Path %s not found in boot image", path);
  grub_free(data);
  return grub_errno;

report:
  data->component = i;
  data->disk = file->device->disk;
  file->data = data;
  file->not_easily_seekable = 0;
  file->offset = 0;
  return GRUB_ERR_NONE;
}
#undef CHECK_SIZE_AND_REPORT

/* Read */
static grub_ssize_t
android_boot_read(grub_file_t file, char* buf, grub_size_t len) {
  struct android_boot_file_data* data = file->data;
  union boot_img_hdr_union hdr;
  grub_uint32_t page_size = 0;
  grub_uint64_t offset = 0;
  const char* tmp;
  grub_ssize_t ret;

  if (data->component == COMPONENT_INFO) {
    if (data->buf) {
      grub_memcpy(buf, data->buf + file->offset, len);
      return len;
    } else {
      return -1;
    }
  }

  if (grub_disk_read(file->device->disk, 0, 0, sizeof(hdr), &hdr)) {
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_READ_ERROR,
               "Failed to load boot image header from disk");
    return -1;
  }

  if (grub_memcmp(hdr.v0.magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0) {
    if (hdr.v0.header_version <= 2) {
      offset = page_size = hdr.v0.page_size;

      // kernel
      if (data->component == COMPONENT_KERNEL)
        goto read;
      offset += ALIGN_UP(hdr.v0.kernel_size, page_size);

      // ramdisk
      if (data->component == COMPONENT_RAMDISK)
        goto read;
      offset += ALIGN_UP(hdr.v0.ramdisk_size, page_size);

      // second (optional)
      if (data->component == COMPONENT_SECOND)
        goto read;
      offset += ALIGN_UP(hdr.v0.second_size, page_size);

      if (hdr.v0.header_version >= 1) {
        // recovery dtbo
        if (data->component == COMPONENT_RECOVERY_DTBO)
          goto read;
        offset += ALIGN_UP(hdr.v1.recovery_dtbo_size, page_size);
      }

      if (hdr.v0.header_version >= 2) {
        // dtb
        if (data->component == COMPONENT_DTB)
          goto read;
        offset += ALIGN_UP(hdr.v2.dtb_size, page_size);
      }
    } else if (hdr.v3.header_version <= 4) {
      offset = 4096;

      tmp = grub_env_get("android_vendor_boot_img_page_size");
      if (tmp)
        page_size = grub_strtoull(tmp, 0, 0);
      if (!tmp || !page_size) {
        grub_printf("%s: %d\n", __func__, __LINE__);
        grub_error(GRUB_ERR_BAD_ARGUMENT,
                   "Unable to get android boot image page size from env");
        return -1;
      }

      // kernel
      if (data->component == COMPONENT_KERNEL)
        goto read;
      offset += ALIGN_UP(hdr.v0.kernel_size, page_size);

      // ramdisk
      if (data->component == COMPONENT_RAMDISK)
        goto read;
      offset += ALIGN_UP(hdr.v0.ramdisk_size, page_size);
    }
  } else if (grub_memcmp(hdr.v0.magic, VENDOR_BOOT_MAGIC,
                         VENDOR_BOOT_MAGIC_SIZE)
             == 0) {
    // TODO
  } else {
    grub_printf("%s: %d\n", __func__, __LINE__);
    grub_error(GRUB_ERR_UNKNOWN_FS, "Invalid Android boot image magic");
    return -1;
  }

read:

/*
grub_printf("kernel_size=%u\n", hdr.v0.kernel_size);
grub_printf("kernel_addr=%u\n", hdr.v0.kernel_addr);
grub_printf("ramdisk_size=%u\n", hdr.v0.ramdisk_size);
grub_printf("ramdisk_addr=%u\n", hdr.v0.ramdisk_addr);
grub_printf("second_size=%u\n", hdr.v0.second_size);
grub_printf("second_addr=%u\n", hdr.v0.second_addr);
grub_printf("tags_addr=%u\n", hdr.v0.tags_addr);
grub_printf("page_size=%u\n", hdr.v0.page_size);
grub_printf("header_version=%u\n", hdr.v0.header_version);
grub_printf("name=%s\n", hdr.v0.name);
grub_printf("cmdline=%s\n", hdr.v0.cmdline);
*/

  offset += file->offset;
  data->disk->read_hook = file->read_hook;
  data->disk->read_hook_data = file->read_hook_data;
  //grub_printf("offset=%lu+%lu, len=%lu\n", offset, file->offset, len);
  ret = grub_disk_read(data->disk, 0, offset, len, buf) ? -1 : (grub_ssize_t)len;
  data->disk->read_hook = 0;

  return ret;
}

/* Close */
static grub_err_t
android_boot_close(grub_file_t file) {
  struct android_boot_file_data* data = file->data;

  if (data->buf)
    grub_free(data->buf);

  grub_free(data);
  return GRUB_ERR_NONE;
}

/* Register FS */
static struct grub_fs android_boot_fs = {
  .name = "android_bootimg",
  .fs_dir = android_boot_dir,
  .fs_open = android_boot_open,
  .fs_read = android_boot_read,
  .fs_close = android_boot_close,
#ifdef GRUB_UTIL
  .reserved_first_sector = 0,
  .blocklist_install = 0,
#endif
};

GRUB_MOD_INIT(android_bootimg) {
  grub_fs_register(&android_boot_fs);
}

GRUB_MOD_FINI(android_bootimg) {
  grub_fs_unregister(&android_boot_fs);
}
