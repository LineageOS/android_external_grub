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

#include <grub/command.h>
#include <grub/dl.h>
#include <grub/disk.h>
#include <grub/env.h>
#include <grub/file.h>
#include <grub/misc.h>
#include <grub/partition.h>

GRUB_MOD_LICENSE("GPLv3+");

static grub_err_t
loadstring_command(grub_command_t cmd __attribute__((unused)), int argc,
                   char** argv) {
  char *buf, *device_name;
  grub_off_t offset = 0;
  grub_size_t size;

  if (argc < 3) {
    return grub_error(GRUB_ERR_BAD_ARGUMENT,
                      "Usage: loadstring <device> <variable> <size> [offset]");
  }

  size = grub_strtoul(argv[2], 0, 0);
  if (!size) {
    return grub_error(GRUB_ERR_BAD_NUMBER, "Invalid size");
  }

  if (argc >= 3) {
    offset = grub_strtoul(argv[3], 0, 0);
  }

  device_name = grub_file_get_device_name(argv[0]);
  grub_device_t device = grub_device_open(device_name);
  grub_free(device_name);
  if (!device) {
    return grub_error(GRUB_ERR_UNKNOWN_DEVICE, "Failed to open device %s",
                      argv[0]);
  }

  if (!device->disk) {
    grub_device_close(device);
    return grub_error(GRUB_ERR_FILE_NOT_FOUND, "Device %s is not disk",
                      argv[0]);
  }

  buf = grub_malloc(size + 1);
  if (!buf) {
    return grub_error(GRUB_ERR_OUT_OF_MEMORY,
                      "Failed to allocate memory for buffer");
  }

  buf[size] = '\0';

  if (grub_disk_read(device->disk, 0, offset, size, buf)) {
    return grub_error(GRUB_ERR_READ_ERROR, "Failed to read from disk");
  }

  grub_device_close(device);

  grub_env_set(argv[1], buf);

  grub_free(buf);

  return GRUB_ERR_NONE;
}

static grub_command_t cmd;

GRUB_MOD_INIT(loadstring) {
  cmd = grub_register_command("loadstring", loadstring_command,
                              "loadstring <device> <variable> <size> [offset]",
                              "Load string from partition");
}

GRUB_MOD_FINI(loadstring) {
  grub_unregister_command(cmd);
}
