/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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

struct vendor_boot_img_hdr_v3 {
  grub_uint8_t magic[VENDOR_BOOT_MAGIC_SIZE];

  grub_uint32_t header_version;

  grub_uint32_t page_size;

  grub_uint32_t kernel_addr;
  grub_uint32_t ramdisk_addr;

  grub_uint32_t vendor_ramdisk_size;

  grub_uint8_t cmdline[VENDOR_BOOT_ARGS_SIZE];

  grub_uint32_t tags_addr;
  grub_uint8_t name[VENDOR_BOOT_NAME_SIZE];

  grub_uint32_t header_size;

  grub_uint32_t dtb_size;
  grub_uint64_t dtb_addr;
} __attribute__((packed));

struct vendor_boot_img_hdr_v4 {
  struct vendor_boot_img_hdr_v3 v3;
  grub_uint32_t vendor_ramdisk_table_size;
  grub_uint32_t vendor_ramdisk_table_entry_num;
  grub_uint32_t vendor_ramdisk_table_entry_size;
  grub_uint32_t bootconfig_size;
} __attribute__((packed));

union vendor_boot_img_hdr_union {
  struct vendor_boot_img_hdr_v3 v3;
  struct vendor_boot_img_hdr_v4 v4;
} __attribute__((packed));

struct vendor_ramdisk_table_entry_v4 {
  grub_uint32_t ramdisk_size;
  grub_uint32_t ramdisk_offset;
  grub_uint32_t ramdisk_type;
  grub_uint8_t ramdisk_name[VENDOR_RAMDISK_NAME_SIZE];

  grub_uint32_t board_id[VENDOR_RAMDISK_TABLE_ENTRY_BOARD_ID_SIZE];
} __attribute__((packed));

struct vendor_ramdisk_fragment_entry {
  struct vendor_ramdisk_table_entry_v4 tbl_v4;
  grub_uint64_t offset_from_disk;
  struct vendor_ramdisk_fragment_entry* next;
};

enum android_boot_img_component {
  COMPONENT_NONE = 0,
  COMPONENT_KERNEL,
  COMPONENT_RAMDISK,
  COMPONENT_SECOND,
  COMPONENT_RECOVERY_DTBO,
  COMPONENT_DTB,
  COMPONENT_INFO,
  COMPONENT_COUNT,
};

enum android_vendor_boot_img_component {
  VENDOR_COMPONENT_NONE = 0,
  VENDOR_COMPONENT_VENDOR_RAMDISK,
  VENDOR_COMPONENT_DTB,
  VENDOR_COMPONENT_BOOTCONFIG,
  VENDOR_COMPONENT_INFO,
  VENDOR_COMPONENT_COUNT,
};

static const char* android_boot_img_component_filename[COMPONENT_COUNT] = {
  [COMPONENT_KERNEL] = "kernel", [COMPONENT_RAMDISK] = "ramdisk.img",
  [COMPONENT_SECOND] = "second", [COMPONENT_RECOVERY_DTBO] = "recovery_dtbo",
  [COMPONENT_DTB] = "dtb",       [COMPONENT_INFO] = "boot-info.cfg"
};

static const char*
    android_vendor_boot_img_component_filename[VENDOR_COMPONENT_COUNT]
    = {
        [VENDOR_COMPONENT_VENDOR_RAMDISK] = "vendor_ramdisk.img",
        [VENDOR_COMPONENT_DTB] = "dtb",
        [VENDOR_COMPONENT_BOOTCONFIG] = "bootconfig.txt",
        [VENDOR_COMPONENT_INFO] = "vendor_boot-info.cfg",
      };

struct android_boot_file_data {
  grub_disk_t disk;
  enum android_boot_img_component component;
  enum android_vendor_boot_img_component vendor_component;
  char* content;
};

/* COMPONENT_INFO content */
#define COMPONENT_INFO_FMT_BOOT_V0_V1_V2                                      \
  "set android_boot_img_name='%s'\n"                                          \
  "set android_boot_img_cmdline='%s'\n"

static char*
get_info_content_boot(union boot_img_hdr_union* hdr) {
  grub_uint32_t ver = hdr->v0.header_version;
  if (ver <= 2) {
    return grub_xasprintf(COMPONENT_INFO_FMT_BOOT_V0_V1_V2, hdr->v0.name,
                          hdr->v0.cmdline);
  }
  return NULL;
}

#define COMPONENT_INFO_FMT_VENDOR_BOOT_V3_V4                                  \
  "set android_vendor_boot_img_cmdline='%s'\n"                                \
  "set android_vendor_boot_img_name='%s'\n"

static char*
get_info_content_vendor_boot(union vendor_boot_img_hdr_union* vhdr) {
  grub_uint32_t ver = vhdr->v3.header_version;
  if (ver <= 4) {
    return grub_xasprintf(COMPONENT_INFO_FMT_VENDOR_BOOT_V3_V4,
                          vhdr->v3.cmdline, vhdr->v3.name);
  }
  return NULL;
}

/* Helpers */
static inline bool
check_boot_img_header(grub_device_t device) {
  char buf[BOOT_MAGIC_SIZE];
  if (grub_disk_read(device->disk, 0, 0, sizeof(buf), &buf))
    return false;
  return grub_memcmp(buf, BOOT_MAGIC, BOOT_MAGIC_SIZE) == 0;
}

static inline bool
check_vendor_boot_img_header(grub_device_t device) {
  char buf[VENDOR_BOOT_MAGIC_SIZE];
  if (grub_disk_read(device->disk, 0, 0, sizeof(buf), &buf))
    return false;
  return grub_memcmp(buf, VENDOR_BOOT_MAGIC, VENDOR_BOOT_MAGIC_SIZE) == 0;
}

static grub_ssize_t
get_boot_img_component_size(union boot_img_hdr_union* hdr,
                            enum android_boot_img_component component) {
  grub_uint32_t ver = hdr->v0.header_version;
  grub_ssize_t tmp_size;
  char* tmp_charp;
  switch (component) {
    case COMPONENT_KERNEL:
      if (ver <= 2)
        return hdr->v0.kernel_size;
      else if (ver <= 4)
        return hdr->v3.kernel_size;
      break;
    case COMPONENT_RAMDISK:
      if (ver <= 2)
        return hdr->v0.ramdisk_size;
      else if (ver <= 4)
        return hdr->v3.ramdisk_size;
      break;
    case COMPONENT_SECOND:
      if (ver <= 2)
        return hdr->v0.second_size;
      break;
    case COMPONENT_RECOVERY_DTBO:
      if (ver >= 1 && ver <= 2)
        return hdr->v1.recovery_dtbo_size;
      break;
    case COMPONENT_DTB:
      if (ver == 2)
        return hdr->v2.dtb_size;
      break;
    case COMPONENT_INFO:
      tmp_charp = get_info_content_boot(hdr);
      if (tmp_charp) {
        tmp_size = grub_strlen(tmp_charp);
        grub_free(tmp_charp);
        return tmp_size;
      }
      break;
    case COMPONENT_NONE:
      [[fallthrough]];
    case COMPONENT_COUNT:
      break;
  }
  return 0;
}

static grub_ssize_t
get_vendor_boot_img_component_size(
    union vendor_boot_img_hdr_union* vhdr,
    enum android_vendor_boot_img_component vendor_component) {
  grub_uint32_t ver = vhdr->v3.header_version;
  grub_ssize_t tmp_size;
  char* tmp_charp;
  switch (vendor_component) {
    case VENDOR_COMPONENT_VENDOR_RAMDISK:
      if (ver <= 4)
        return vhdr->v3.vendor_ramdisk_size;
      break;
    case VENDOR_COMPONENT_DTB:
      if (ver <= 4)
        return vhdr->v3.dtb_size;
      break;
    case VENDOR_COMPONENT_BOOTCONFIG:
      if (ver == 4)
        return vhdr->v4.bootconfig_size;
      break;
    case VENDOR_COMPONENT_INFO:
      tmp_charp = get_info_content_vendor_boot(vhdr);
      if (tmp_charp) {
        tmp_size = grub_strlen(tmp_charp);
        grub_free(tmp_charp);
        return tmp_size;
      }
      break;
    case VENDOR_COMPONENT_NONE:
      [[fallthrough]];
    case VENDOR_COMPONENT_COUNT:
      break;
  }
  return 0;
}

/* vendor ramdisk fragments handling*/
static char*
local_strappend(char* dest, const char* src) {
  grub_size_t len_dest = dest ? grub_strlen(dest) : 0;
  grub_size_t len_src = grub_strlen(src);

  char* newbuf = grub_realloc(dest, len_dest + len_src + 1);
  if (!newbuf)
    return NULL;

  grub_memcpy(newbuf + len_dest, src, len_src + 1);  // Copy + null terminator
  return newbuf;
}

static char*
get_vendor_ramdisk_fragment_filename(
    struct vendor_ramdisk_table_entry_v4* tbl_v4) {
  char* result = grub_strdup("vendor_ramdisk-");
  local_strappend(result, (const char*)&tbl_v4->ramdisk_name);
  local_strappend(result, "-");
  switch (tbl_v4->ramdisk_type) {
    case VENDOR_RAMDISK_TYPE_NONE:
      local_strappend(result, "NONE");
      break;
    case VENDOR_RAMDISK_TYPE_PLATFORM:
      local_strappend(result, "PLATFORM");
      break;
    case VENDOR_RAMDISK_TYPE_RECOVERY:
      local_strappend(result, "RECOVERY");
      break;
    case VENDOR_RAMDISK_TYPE_DLKM:
      local_strappend(result, "DLKM");
      break;
    default:
      local_strappend(result, "UNKNOWN");
      break;
  }
  local_strappend(result, ".img");
  return result;
}

static struct vendor_ramdisk_fragment_entry*
get_vendor_ramdisk_fragment_entries(grub_device_t device) {
  union vendor_boot_img_hdr_union vhdr;
  unsigned int i;
  struct vendor_ramdisk_fragment_entry *result, *result_first, *result_prev;
  grub_uint32_t page_size = 0;
  grub_uint64_t offset = 0, section_offset = 0;

  if (grub_disk_read(device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
    grub_error(GRUB_ERR_BAD_FS,
               "Failed to load vendor_boot image header from disk");
    return NULL;
  }

  if (vhdr.v3.header_version < 4) {
    grub_error(GRUB_ERR_BAD_FS, "vendor_boot image header version < 4 does "
                                "not support vendor ramdisk fragment");
    return NULL;
  }

  if (!vhdr.v3.vendor_ramdisk_size
      || !vhdr.v4.vendor_ramdisk_table_entry_num) {
    grub_error(GRUB_ERR_BAD_FS, "No vendor ramdisk fragment");
    return NULL;
  }

  page_size = vhdr.v3.page_size;
  if (vhdr.v3.header_version == 3)
    offset = ALIGN_UP(2112, page_size);
  else
    offset = ALIGN_UP(2128, page_size);
  section_offset = offset;
  offset += ALIGN_UP(vhdr.v3.vendor_ramdisk_size, page_size);
  offset += ALIGN_UP(vhdr.v3.dtb_size, page_size);

  for (i = 0; i < vhdr.v4.vendor_ramdisk_table_entry_num; i++) {
    result_prev = result;
    result = grub_zalloc(sizeof(*result));
    if (!result) {
      grub_error(GRUB_ERR_BAD_FS, "Unable to allocate memory");
      break;
    }

    if (grub_disk_read(device->disk, 0, offset, sizeof(result->tbl_v4),
                       &result->tbl_v4)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load vendor ramdisk table from disk");
      break;
    }

    result->offset_from_disk = section_offset + result->tbl_v4.ramdisk_offset;

    if (i == 0) {
      result_first = result;
    } else {
      result_prev->next = result;
    }

    offset += vhdr.v4.vendor_ramdisk_table_entry_size;
    if (offset >= vhdr.v4.vendor_ramdisk_table_size)
      break;
  }

  return result_first;
}

static void
free_vendor_ramdisk_fragment_entries(
    struct vendor_ramdisk_fragment_entry* vrfe) {
  struct vendor_ramdisk_fragment_entry* cur_vrfe;
  while (vrfe) {
    cur_vrfe = vrfe;
    vrfe = vrfe->next;
    grub_free(cur_vrfe);
  }
}

/* Directory listing */
static grub_err_t
android_bootimg_dir(grub_device_t device,
                    const char* path __attribute__((unused)),
                    grub_fs_dir_hook_t hook, void* hook_data) {
  grub_ssize_t tmp_size;
  struct grub_dirhook_info info;
  union boot_img_hdr_union hdr;
  union vendor_boot_img_hdr_union vhdr;
  unsigned int i;

  if (check_boot_img_header(device)) {
    if (grub_disk_read(device->disk, 0, 0, sizeof(hdr), &hdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load boot image header from disk");
      return grub_errno;
    }
    for (i = 0; i < COMPONENT_COUNT; i++) {
      tmp_size = get_boot_img_component_size(&hdr, i);
      if (tmp_size) {
        grub_memset(&info, 0, sizeof(info));
        hook(android_boot_img_component_filename[i], &info, hook_data);
      }
    }
  } else if (check_vendor_boot_img_header(device)) {
    if (grub_disk_read(device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load vendor_boot image header from disk");
      return grub_errno;
    }
    for (i = 0; i < VENDOR_COMPONENT_COUNT; i++) {
      tmp_size = get_vendor_boot_img_component_size(&vhdr, i);
      if (tmp_size) {
        grub_memset(&info, 0, sizeof(info));
        hook(android_vendor_boot_img_component_filename[i], &info, hook_data);
      }
    }
  } else {
    grub_error(GRUB_ERR_BAD_FS, "Invalid Android boot image");
    return grub_errno;
  }

  return GRUB_ERR_NONE;
}

/* Open a file */
static grub_err_t
android_bootimg_open(grub_file_t file, const char* path) {
  grub_ssize_t component_size;
  struct android_boot_file_data* data;
  unsigned int i;
  union boot_img_hdr_union hdr;
  union vendor_boot_img_hdr_union vhdr;

  data = grub_zalloc(sizeof(*data));
  if (!data) {
    grub_error(GRUB_ERR_BAD_FS, "Unable to allocate memory");
    return grub_errno;
  }

  if (check_boot_img_header(file->device)) {
    if (grub_disk_read(file->device->disk, 0, 0, sizeof(hdr), &hdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load boot image header from disk");
      return grub_errno;
    }
    for (i = 0; i < COMPONENT_COUNT; i++) {
      if (!grub_strcmp(path + 1, android_boot_img_component_filename[i])) {
        component_size = get_boot_img_component_size(&hdr, i);
        if (component_size) {
          data->component = i;
          goto found;
        }
      }
    }
    grub_error(GRUB_ERR_BAD_FS, "Path %s is unsupported for boot", path);
    return grub_errno;
  } else if (check_vendor_boot_img_header(file->device)) {
    if (grub_disk_read(file->device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load vendor_boot image header from disk");
      return grub_errno;
    }
    for (i = 0; i < VENDOR_COMPONENT_COUNT; i++) {
      if (!grub_strcmp(path + 1,
                       android_vendor_boot_img_component_filename[i])) {
        component_size = get_vendor_boot_img_component_size(&vhdr, i);
        if (component_size) {
          data->vendor_component = i;
          goto found;
        }
      }
    }
    grub_error(GRUB_ERR_BAD_FS, "Path %s is unsupported for vendor_boot",
               path);
    return grub_errno;
  } else {
    grub_free(data);
    grub_error(GRUB_ERR_BAD_FS, "Invalid Android boot image");
    return grub_errno;
  }

found:
  if (data->component == COMPONENT_INFO) {
    data->content = get_info_content_boot(&hdr);
  } else if (data->vendor_component == VENDOR_COMPONENT_INFO) {
    data->content = get_info_content_vendor_boot(&vhdr);
  }
  data->disk = file->device->disk;
  file->data = data;
  file->not_easily_seekable = 0;
  file->offset = 0;
  file->size = component_size;
  return GRUB_ERR_NONE;
}

/* Read */
static grub_ssize_t
android_bootimg_read(grub_file_t file, char* buf, grub_size_t len) {
  struct android_boot_file_data* data = file->data;
  union boot_img_hdr_union hdr;
  union vendor_boot_img_hdr_union vhdr;
  grub_uint32_t page_size = 0;
  grub_uint64_t offset = 0;
  grub_ssize_t ret;

  if (data->component == COMPONENT_INFO
      || data->vendor_component == VENDOR_COMPONENT_INFO) {
    if (data->content) {
      grub_memcpy(buf, data->content + file->offset, len);
      return len;
    } else {
      return -1;
    }
  }

  if (check_boot_img_header(file->device)) {
    if (grub_disk_read(file->device->disk, 0, 0, sizeof(hdr), &hdr)) {
      grub_error(GRUB_ERR_READ_ERROR,
                 "Failed to load boot image header from disk");
      return -1;
    }

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

      // second
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

      // kernel
      if (data->component == COMPONENT_KERNEL)
        goto read;
      offset += ALIGN_UP(hdr.v3.kernel_size, 4096);

      // ramdisk
      if (data->component == COMPONENT_RAMDISK)
        goto read;
      offset += ALIGN_UP(hdr.v3.ramdisk_size, 4096);

      if (hdr.v3.header_version >= 4) {
        // boot signature
        offset += ALIGN_UP(hdr.v4.signature_size, 4096);
      }
    }
  } else if (check_vendor_boot_img_header(file->device)) {
    if (grub_disk_read(file->device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load vendor_boot image header from disk");
      return grub_errno;
    }

    page_size = vhdr.v3.page_size;
    if (vhdr.v3.header_version == 3)
      offset = ALIGN_UP(2112, page_size);
    else
      offset = ALIGN_UP(2128, page_size);

    // vendor_ramdisk
    if (data->vendor_component == VENDOR_COMPONENT_VENDOR_RAMDISK)
      goto read;
    offset += ALIGN_UP(vhdr.v3.vendor_ramdisk_size, page_size);

    // dtb
    if (data->vendor_component == VENDOR_COMPONENT_DTB)
      goto read;
    offset += ALIGN_UP(vhdr.v3.dtb_size, page_size);

    if (vhdr.v3.header_version >= 4) {
      // vendor ramdisk table
      offset += ALIGN_UP(vhdr.v4.vendor_ramdisk_table_size, page_size);

      // bootconfig
      if (data->vendor_component == VENDOR_COMPONENT_BOOTCONFIG)
        goto read;
      offset += ALIGN_UP(vhdr.v4.bootconfig_size, page_size);
    }
  } else {
    grub_error(GRUB_ERR_UNKNOWN_FS, "Invalid Android boot image magic");
    return -1;
  }

read:

  offset += file->offset;
  data->disk->read_hook = file->read_hook;
  data->disk->read_hook_data = file->read_hook_data;
  ret = grub_disk_read(data->disk, 0, offset, len, buf) ? -1
                                                        : (grub_ssize_t)len;
  data->disk->read_hook = 0;

  return ret;
}

/* Close */
static grub_err_t
android_bootimg_close(grub_file_t file) {
  struct android_boot_file_data* data = file->data;

  if (data->content)
    grub_free(data->content);

  grub_free(data);
  return GRUB_ERR_NONE;
}

/* Register FS */
static struct grub_fs android_bootimg_fs = {
  .name = "android_bootimg_fs",
  .fs_dir = android_bootimg_dir,
  .fs_open = android_bootimg_open,
  .fs_read = android_bootimg_read,
  .fs_close = android_bootimg_close,
#ifdef GRUB_UTIL
  .reserved_first_sector = 0,
  .blocklist_install = 0,
#endif
};

GRUB_MOD_INIT(android_bootimg_fs) {
  grub_fs_register(&android_bootimg_fs);
}

GRUB_MOD_FINI(android_bootimg_fs) {
  grub_fs_unregister(&android_bootimg_fs);
}
