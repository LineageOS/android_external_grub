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

#define BOOTCONFIG_MAGIC "#BOOTCONFIG\n"
#define BOOTCONFIG_MAGIC_LEN 12
#define BOOTCONFIG_ALIGN_SHIFT 2
#define BOOTCONFIG_ALIGN (1 << BOOTCONFIG_ALIGN_SHIFT)
#define BOOTCONFIG_ALIGN_MASK (BOOTCONFIG_ALIGN - 1)

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

struct bootconfig_tail {
  grub_uint32_t size;
  grub_uint32_t csum;
  char magic[BOOTCONFIG_MAGIC_LEN];
} __attribute__((packed));

enum android_boot_img_component {
  COMPONENT_NONE = 0,
  // Direct read
  COMPONENT_KERNEL,
  COMPONENT_RAMDISK,
  COMPONENT_SECOND,
  COMPONENT_RECOVERY_DTBO,
  COMPONENT_DTB,
  COMPONENT_SIGNATURE,
  // Generated
  COMPONENT_INFO,
  COMPONENT_COUNT,
};

enum android_vendor_boot_img_component {
  VENDOR_COMPONENT_NONE = 0,
  // Direct read
  VENDOR_COMPONENT_VENDOR_RAMDISK,
  VENDOR_COMPONENT_DTB,
  VENDOR_COMPONENT_BOOTCONFIG_TXT,
  VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE,
  // Generated
  VENDOR_COMPONENT_INFO,
  VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO,
  VENDOR_COMPONENT_BOOTCONFIG_BIN,
  VENDOR_COMPONENT_COUNT,
};

static const char* android_boot_img_component_filename[COMPONENT_COUNT]
    = { [COMPONENT_KERNEL] = "kernel",
        [COMPONENT_RAMDISK] = "ramdisk.img",
        [COMPONENT_SECOND] = "second",
        [COMPONENT_RECOVERY_DTBO] = "recovery_dtbo",
        [COMPONENT_DTB] = "dtb",
        [COMPONENT_SIGNATURE] = "signature",
        [COMPONENT_INFO] = "boot-info.cfg" };

static const char*
    android_vendor_boot_img_component_filename[VENDOR_COMPONENT_COUNT]
    = {
        [VENDOR_COMPONENT_VENDOR_RAMDISK] = "vendor_ramdisk.img",
        [VENDOR_COMPONENT_DTB] = "dtb",
        [VENDOR_COMPONENT_BOOTCONFIG_BIN] = "bootconfig.bin",
        [VENDOR_COMPONENT_BOOTCONFIG_TXT] = "bootconfig.txt",
        [VENDOR_COMPONENT_INFO] = "vendor_boot-info.cfg",
        [VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO]
        = "vendor_boot-ramdisk-fragment-info.cfg",
      };

struct android_boot_file_data {
  grub_disk_t disk;
  grub_uint64_t offset_from_disk;
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

static grub_uint64_t
get_boot_img_component_offset(union boot_img_hdr_union* hdr,
                              enum android_boot_img_component component) {
  grub_uint32_t page_size = 0;
  grub_uint64_t offset = 0;

  if (hdr->v0.header_version <= 2) {
    offset = page_size = hdr->v0.page_size;

    // kernel
    if (component == COMPONENT_KERNEL)
      return offset;
    offset += ALIGN_UP(hdr->v0.kernel_size, page_size);

    // ramdisk
    if (component == COMPONENT_RAMDISK)
      return offset;
    offset += ALIGN_UP(hdr->v0.ramdisk_size, page_size);

    // second
    if (component == COMPONENT_SECOND)
      return offset;
    offset += ALIGN_UP(hdr->v0.second_size, page_size);

    if (hdr->v0.header_version >= 1) {
      // recovery dtbo
      if (component == COMPONENT_RECOVERY_DTBO)
        return offset;
      offset += ALIGN_UP(hdr->v1.recovery_dtbo_size, page_size);
    }

    if (hdr->v0.header_version >= 2) {
      // dtb
      if (component == COMPONENT_DTB)
        return offset;
      offset += ALIGN_UP(hdr->v2.dtb_size, page_size);
    }
  } else if (hdr->v3.header_version <= 4) {
    offset = 4096;

    // kernel
    if (component == COMPONENT_KERNEL)
      return offset;
    offset += ALIGN_UP(hdr->v3.kernel_size, 4096);

    // ramdisk
    if (component == COMPONENT_RAMDISK)
      return offset;
    offset += ALIGN_UP(hdr->v3.ramdisk_size, 4096);

    if (hdr->v3.header_version == 4) {
      // boot signature
      if (component == COMPONENT_SIGNATURE)
        return offset;
      offset += ALIGN_UP(hdr->v4.signature_size, 4096);
    }
  }

  return 0;
}

static grub_ssize_t
get_boot_img_component_size(union boot_img_hdr_union* hdr,
                            enum android_boot_img_component component) {
  grub_uint32_t ver = hdr->v0.header_version;
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
    case COMPONENT_SIGNATURE:
      if (ver == 4)
        return hdr->v4.signature_size;
      break;
    case COMPONENT_INFO:
      // handled separately
      [[fallthrough]];
    case COMPONENT_NONE:
      [[fallthrough]];
    case COMPONENT_COUNT:
      break;
  }
  return 0;
}

static grub_ssize_t
get_vendor_boot_img_component_offset(
    union vendor_boot_img_hdr_union* vhdr,
    enum android_vendor_boot_img_component vendor_component) {
  grub_uint32_t page_size = 0;
  grub_uint64_t offset = 0;

  page_size = vhdr->v3.page_size;
  if (vhdr->v3.header_version == 3)
    offset = ALIGN_UP(2112, page_size);
  else
    offset = ALIGN_UP(2128, page_size);

  // vendor_ramdisk
  if (vendor_component == VENDOR_COMPONENT_VENDOR_RAMDISK)
    return offset;
  offset += ALIGN_UP(vhdr->v3.vendor_ramdisk_size, page_size);

  // dtb
  if (vendor_component == VENDOR_COMPONENT_DTB)
    return offset;
  offset += ALIGN_UP(vhdr->v3.dtb_size, page_size);

  if (vhdr->v3.header_version == 4) {
    // vendor ramdisk table
    if (vendor_component == VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE)
      return offset;
    offset += ALIGN_UP(vhdr->v4.vendor_ramdisk_table_size, page_size);

    // bootconfig
    if (vendor_component == VENDOR_COMPONENT_BOOTCONFIG_TXT)
      return offset;
    offset += ALIGN_UP(vhdr->v4.bootconfig_size, page_size);
  }

  return 0;
}

static grub_ssize_t
get_vendor_boot_img_component_size(
    union vendor_boot_img_hdr_union* vhdr,
    enum android_vendor_boot_img_component vendor_component) {
  grub_uint32_t ver = vhdr->v3.header_version;
  switch (vendor_component) {
    case VENDOR_COMPONENT_VENDOR_RAMDISK:
      if (ver == 3)
        return vhdr->v3.vendor_ramdisk_size;
      break;
    case VENDOR_COMPONENT_DTB:
      if (ver == 3)
        return vhdr->v3.dtb_size;
      break;
    case VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE:
      if (ver == 4)
        return vhdr->v4.vendor_ramdisk_table_size;
      break;
    case VENDOR_COMPONENT_BOOTCONFIG_TXT:
      if (ver == 4)
        return vhdr->v4.bootconfig_size;
      break;
    case VENDOR_COMPONENT_INFO:
      // handled separately
      [[fallthrough]];
    case VENDOR_COMPONENT_BOOTCONFIG_BIN:
      // handled separately
      [[fallthrough]];
    case VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO:
      // handled separately
      [[fallthrough]];
    case VENDOR_COMPONENT_NONE:
      [[fallthrough]];
    case VENDOR_COMPONENT_COUNT:
      break;
  }
  return 0;
}

static inline grub_uint32_t
xbc_calc_checksum(void* data, grub_uint32_t size) {
  unsigned char* p = data;
  grub_uint32_t ret = 0;

  while (size--)
    ret += *p++;

  return ret;
}

static char*
get_vendor_bootconfig_content(grub_device_t device, grub_off_t* out_size) {
  char* result;
  const char magic[BOOTCONFIG_MAGIC_LEN] = BOOTCONFIG_MAGIC;
  struct bootconfig_tail* result_tail;
  union vendor_boot_img_hdr_union vhdr;
  grub_ssize_t bootconfig_size, content_size, pad, total_size;

  *out_size = 0;

  if (grub_disk_read(device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
    grub_error(GRUB_ERR_BAD_FS,
               "Failed to load vendor_boot image header from disk");
    return NULL;
  }

  if (vhdr.v3.header_version != 4) {
    grub_error(GRUB_ERR_BAD_FS, "vendor_boot image header version != 4 does "
                                "not support bootconfig");
    return NULL;
  }

  if (!vhdr.v4.bootconfig_size) {
    grub_error(GRUB_ERR_BAD_FS, "No bootconfig");
    return NULL;
  }

  bootconfig_size = vhdr.v4.bootconfig_size + 2;  // 2 = \n + \0
  total_size = bootconfig_size + sizeof(struct bootconfig_tail);
  pad = ((total_size + BOOTCONFIG_ALIGN - 1) & (~BOOTCONFIG_ALIGN_MASK))
        - total_size;
  content_size = total_size + pad;

  result = grub_zalloc(content_size + 1);  // 1 = \0
  if (!result) {
    grub_error(GRUB_ERR_BAD_FS,
               "Failed to allocate memory for bootconfig content");
    return NULL;
  }

  *out_size = content_size;

  if (grub_disk_read(device->disk, 0,
                     get_vendor_boot_img_component_offset(
                         &vhdr, VENDOR_COMPONENT_BOOTCONFIG_TXT),
                     vhdr.v4.bootconfig_size, result)) {
    grub_error(GRUB_ERR_BAD_FS, "Failed to load bootconfig from disk");
    goto fail;
  }

  result_tail = (struct bootconfig_tail*)(result + bootconfig_size + pad);
  result_tail->size = bootconfig_size;
  result_tail->csum = xbc_calc_checksum(result, result_tail->size);
  grub_memcpy(&result_tail->magic, magic, BOOTCONFIG_MAGIC_LEN);

  return result;

fail:
  grub_free(result);
  return NULL;
}

/* vendor ramdisk fragments handling*/
static char*
get_vendor_ramdisk_fragment_filename(
    struct vendor_ramdisk_fragment_entry* vrfe) {
  char* result = grub_strdup("vendor_ramdisk-");
  switch (vrfe->tbl_v4.ramdisk_type) {
    case VENDOR_RAMDISK_TYPE_NONE:
      result = local_strappend(result, "NONE");
      break;
    case VENDOR_RAMDISK_TYPE_PLATFORM:
      result = local_strappend(result, "PLATFORM");
      break;
    case VENDOR_RAMDISK_TYPE_RECOVERY:
      result = local_strappend(result, "RECOVERY");
      break;
    case VENDOR_RAMDISK_TYPE_DLKM:
      result = local_strappend(result, "DLKM");
      break;
    default:
      result = local_strappend(result, "UNKNOWN");
      break;
  }
  if (grub_strlen((const char*)&vrfe->tbl_v4.ramdisk_name)) {
    result = local_strappend(result, "-");
    result = local_strappend(result, (const char*)&vrfe->tbl_v4.ramdisk_name);
  }
  result = local_strappend(result, ".img");
  return result;
}

static struct vendor_ramdisk_fragment_entry*
get_vendor_ramdisk_fragment_entries(grub_device_t device) {
  union vendor_boot_img_hdr_union vhdr;
  unsigned int i;
  struct vendor_ramdisk_fragment_entry *result = NULL, *result_head = NULL,
                                       *result_prev = NULL;
  grub_uint64_t first_section_offset = 0, section_offset = 0,
                first_table_offset = 0, table_offset = 0;

  if (grub_disk_read(device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
    grub_error(GRUB_ERR_BAD_FS,
               "Failed to load vendor_boot image header from disk");
    return NULL;
  }

  if (vhdr.v3.header_version != 4) {
    grub_error(GRUB_ERR_BAD_FS, "vendor_boot image header version != 4 does "
                                "not support vendor ramdisk fragment");
    return NULL;
  }

  if (!vhdr.v3.vendor_ramdisk_size
      || !vhdr.v4.vendor_ramdisk_table_entry_num) {
    grub_error(GRUB_ERR_BAD_FS, "No vendor ramdisk fragment");
    return NULL;
  }

  first_section_offset = section_offset = get_vendor_boot_img_component_offset(
      &vhdr, VENDOR_COMPONENT_VENDOR_RAMDISK);
  first_table_offset = table_offset = get_vendor_boot_img_component_offset(
      &vhdr, VENDOR_COMPONENT_VENDOR_RAMDISK_TABLE);

  for (i = 0; i < vhdr.v4.vendor_ramdisk_table_entry_num; i++) {
    result_prev = result;
    result = grub_zalloc(sizeof(*result));
    if (!result) {
      grub_error(GRUB_ERR_BAD_FS, "Unable to allocate memory");
      break;
    }

    if (grub_disk_read(device->disk, 0, table_offset, sizeof(result->tbl_v4),
                       &result->tbl_v4)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load vendor ramdisk table from disk");
      break;
    }

    result->offset_from_disk
        = first_section_offset + result->tbl_v4.ramdisk_offset;

    if (i == 0) {
      result_head = result;
    } else {
      result_prev->next = result;
    }

    table_offset += vhdr.v4.vendor_ramdisk_table_entry_size;
    if (table_offset
        >= first_table_offset + vhdr.v4.vendor_ramdisk_table_size) {
      break;
    }
  }

  return result_head;
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

#define VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO_FMT                     \
  "set android_vendor_boot_img_ramdisk_fragment_none_list='%s'\n"             \
  "set android_vendor_boot_img_ramdisk_fragment_platform_list='%s'\n"         \
  "set android_vendor_boot_img_ramdisk_fragment_recovery_list='%s'\n"         \
  "set android_vendor_boot_img_ramdisk_fragment_dlkm_list='%s'\n"

static char*
get_vendor_ramdisk_fragment_info_content(grub_device_t device) {
  char *buf_list_none, *buf_list_platform, *buf_list_recovery, *buf_list_dlkm,
      *result, *tmp_charp, **dest_buf;
  struct vendor_ramdisk_fragment_entry *vrfe, *vrfe_head;

  buf_list_none = grub_zalloc(1);
  buf_list_platform = grub_zalloc(1);
  buf_list_recovery = grub_zalloc(1);
  buf_list_dlkm = grub_zalloc(1);

  vrfe = vrfe_head = get_vendor_ramdisk_fragment_entries(device);
  while (vrfe) {
    switch (vrfe->tbl_v4.ramdisk_type) {
      case VENDOR_RAMDISK_TYPE_NONE:
        dest_buf = &buf_list_none;
        break;
      case VENDOR_RAMDISK_TYPE_PLATFORM:
        dest_buf = &buf_list_platform;
        break;
      case VENDOR_RAMDISK_TYPE_RECOVERY:
        dest_buf = &buf_list_recovery;
        break;
      case VENDOR_RAMDISK_TYPE_DLKM:
        dest_buf = &buf_list_dlkm;
        break;
      default:
        vrfe = vrfe->next;
        continue;
    }
    *dest_buf = local_strappend(*dest_buf, "(");
    *dest_buf = local_strappend(*dest_buf, device->disk->name);
    *dest_buf = local_strappend(*dest_buf, ")/");

    tmp_charp = get_vendor_ramdisk_fragment_filename(vrfe);
    *dest_buf = local_strappend(*dest_buf, tmp_charp);
    grub_free(tmp_charp);

    *dest_buf = local_strappend(*dest_buf, " ");
    vrfe = vrfe->next;
  }
  if (vrfe_head)
    free_vendor_ramdisk_fragment_entries(vrfe_head);

  result = grub_xasprintf(VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO_FMT,
                          buf_list_none, buf_list_platform, buf_list_recovery,
                          buf_list_dlkm);

  grub_free(buf_list_none);
  grub_free(buf_list_platform);
  grub_free(buf_list_recovery);
  grub_free(buf_list_dlkm);

  return result;
}

/* Directory listing */
static grub_err_t
android_bootimg_dir(grub_device_t device,
                    const char* path __attribute__((unused)),
                    grub_fs_dir_hook_t hook, void* hook_data) {
  char* tmp_charp;
  grub_ssize_t tmp_size;
  struct grub_dirhook_info info;
  struct vendor_ramdisk_fragment_entry *vrfe, *vrfe_head;
  union boot_img_hdr_union hdr;
  union vendor_boot_img_hdr_union vhdr;
  unsigned int i;

  if (check_boot_img_header(device)) {
    if (grub_disk_read(device->disk, 0, 0, sizeof(hdr), &hdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load boot image header from disk");
      return grub_errno;
    }
    for (i = 1; i < COMPONENT_COUNT; i++) {
      tmp_size = 0;
      switch (i) {
        case COMPONENT_INFO:
          tmp_charp = get_info_content_boot(&hdr);
          if (tmp_charp) {
            tmp_size = grub_strlen(tmp_charp);
            grub_free(tmp_charp);
          }
          break;
        default:
          tmp_size = get_boot_img_component_size(&hdr, i);
          break;
      }
      tmp_charp = (char*)android_boot_img_component_filename[i];
      if (tmp_charp[0] && tmp_size) {
        grub_memset(&info, 0, sizeof(info));
        hook(tmp_charp, &info, hook_data);
      }
    }
  } else if (check_vendor_boot_img_header(device)) {
    if (grub_disk_read(device->disk, 0, 0, sizeof(vhdr), &vhdr)) {
      grub_error(GRUB_ERR_BAD_FS,
                 "Failed to load vendor_boot image header from disk");
      return grub_errno;
    }
    for (i = 1; i < VENDOR_COMPONENT_COUNT; i++) {
      tmp_size = 0;
      switch (i) {
        case VENDOR_COMPONENT_INFO:
          tmp_charp = get_info_content_vendor_boot(&vhdr);
          if (tmp_charp) {
            tmp_size = grub_strlen(tmp_charp);
            grub_free(tmp_charp);
          }
          break;
        case VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO:
          tmp_charp = get_vendor_ramdisk_fragment_info_content(device);
          if (tmp_charp) {
            tmp_size = grub_strlen(tmp_charp);
            grub_free(tmp_charp);
          }
          break;
        case VENDOR_COMPONENT_BOOTCONFIG_BIN:
          tmp_size = vhdr.v4.bootconfig_size;  // not equal to the real size
                                               // but doesn't matter here
          break;
        default:
          tmp_size = get_vendor_boot_img_component_size(&vhdr, i);
          break;
      }
      tmp_charp = (char*)android_vendor_boot_img_component_filename[i];
      if (tmp_charp[0] && tmp_size) {
        grub_memset(&info, 0, sizeof(info));
        hook(android_vendor_boot_img_component_filename[i], &info, hook_data);
      }
    }
    vrfe = vrfe_head = get_vendor_ramdisk_fragment_entries(device);
    while (vrfe) {
      grub_memset(&info, 0, sizeof(info));
      tmp_charp = get_vendor_ramdisk_fragment_filename(vrfe);
      hook(tmp_charp, &info, hook_data);
      grub_free(tmp_charp);
      vrfe = vrfe->next;
    }
    if (vrfe_head)
      free_vendor_ramdisk_fragment_entries(vrfe_head);
  } else {
    grub_error(GRUB_ERR_BAD_FS, "Invalid Android boot image");
    return grub_errno;
  }

  return GRUB_ERR_NONE;
}

/* Open a file */
static grub_err_t
android_bootimg_open(grub_file_t file, const char* path) {
  char* tmp_charp;
  struct android_boot_file_data* data;
  struct vendor_ramdisk_fragment_entry *vrfe, *vrfe_head;
  unsigned int i;
  union boot_img_hdr_union hdr;
  union vendor_boot_img_hdr_union vhdr;

  file->size = 0;

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
    for (i = 1; i < COMPONENT_COUNT; i++) {
      if (!grub_strcmp(path + 1, android_boot_img_component_filename[i])) {
        switch (i) {
          case COMPONENT_INFO:
            data->content = get_info_content_boot(&hdr);
            if (data->content) {
              file->size = grub_strlen(data->content);
            }
            break;
          default:
            data->offset_from_disk = get_boot_img_component_offset(&hdr, i);
            file->size = get_boot_img_component_size(&hdr, i);
            break;
        }
        if (file->size) {
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
    for (i = 1; i < VENDOR_COMPONENT_COUNT; i++) {
      if (!grub_strcmp(path + 1,
                       android_vendor_boot_img_component_filename[i])) {
        switch (i) {
          case VENDOR_COMPONENT_INFO:
            data->content = get_info_content_vendor_boot(&vhdr);
            if (data->content) {
              file->size = grub_strlen(data->content);
            }
            break;
          case VENDOR_COMPONENT_BOOTCONFIG_BIN:
            data->content
                = get_vendor_bootconfig_content(file->device, &file->size);
            break;
          case VENDOR_COMPONENT_VENDOR_RAMDISK_FRAGMENT_INFO:
            data->content
                = get_vendor_ramdisk_fragment_info_content(file->device);
            if (data->content) {
              file->size = grub_strlen(data->content);
            }
            break;
          default:
            data->offset_from_disk
                = get_vendor_boot_img_component_offset(&vhdr, i);
            file->size = get_vendor_boot_img_component_size(&vhdr, i);
            break;
        }
        if (file->size) {
          goto found;
        }
      }
    }
    vrfe = vrfe_head = get_vendor_ramdisk_fragment_entries(file->device);
    while (vrfe) {
      tmp_charp = get_vendor_ramdisk_fragment_filename(vrfe);
      if (!grub_strcmp(path + 1, tmp_charp)) {
        grub_free(tmp_charp);
        data->offset_from_disk = vrfe->offset_from_disk;
        file->size = vrfe->tbl_v4.ramdisk_size;
        free_vendor_ramdisk_fragment_entries(vrfe_head);
        goto found;
      }
      grub_free(tmp_charp);
      vrfe = vrfe->next;
    }
    if (vrfe_head)
      free_vendor_ramdisk_fragment_entries(vrfe_head);
    grub_error(GRUB_ERR_BAD_FS, "Path %s is unsupported for vendor_boot",
               path);
    return grub_errno;
  } else {
    grub_free(data);
    grub_error(GRUB_ERR_BAD_FS, "Invalid Android boot image");
    return grub_errno;
  }

found:
  data->disk = file->device->disk;
  file->data = data;
  file->not_easily_seekable = 0;
  file->offset = 0;
  return GRUB_ERR_NONE;
}

/* Read */
static grub_ssize_t
android_bootimg_read(grub_file_t file, char* buf, grub_size_t len) {
  struct android_boot_file_data* data = file->data;
  grub_uint64_t offset = 0;
  grub_ssize_t ret;

  if (data->content) {
    grub_memcpy(buf, data->content + file->offset, len);
    return len;
  } else if (data->offset_from_disk) {
    offset = data->offset_from_disk + file->offset;
    data->disk->read_hook = file->read_hook;
    data->disk->read_hook_data = file->read_hook_data;
    ret = grub_disk_read(data->disk, 0, offset, len, buf) ? -1
                                                          : (grub_ssize_t)len;
    data->disk->read_hook = 0;
  } else {
    grub_error(GRUB_ERR_UNKNOWN_FS, "No content available");
    return -1;
  }

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
