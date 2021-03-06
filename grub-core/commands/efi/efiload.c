/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <grub/dl.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/err.h>
#include <grub/extcmd.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/efi/efiload.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options_efiload[] = {
  {"nc", 'n', 0, N_("Loads the driver, but does not connect the driver."), 0, 0},
  {0, 0, 0, 0, 0, 0}
};

static grub_efi_guid_t loaded_image_protocol_guid = GRUB_EFI_LOADED_IMAGE_PROTOCOL_GUID;

grub_efi_status_t
connect_all_efi (void)
{
  grub_efi_status_t status;
  grub_efi_uintn_t handle_count;
  grub_efi_handle_t *handle_buffer;
  grub_efi_uintn_t index;
  grub_efi_boot_services_t *b;

  b = grub_efi_system_table->boot_services;
  status = efi_call_5 (b->locate_handle_buffer,
                       GRUB_EFI_ALL_HANDLES,
                       NULL, NULL,
                       &handle_count, &handle_buffer);

  if (status != GRUB_EFI_SUCCESS)
    return status;

  for (index = 0; index < handle_count; index++)
  {
    status = efi_call_4 (b->connect_controller, handle_buffer[index], NULL, NULL, 1);
  }

  if (handle_buffer != NULL)
    efi_call_1 (b->free_pool, handle_buffer);

  return GRUB_EFI_SUCCESS;
}

grub_err_t
grub_efi_driver_load (grub_size_t size, void *boot_image, int connect)
{
  grub_efi_status_t status;
  grub_efi_handle_t driver_handle;
  grub_efi_boot_services_t *b;
  grub_efi_loaded_image_t *loaded_image;

  b = grub_efi_system_table->boot_services;
  status = efi_call_6 (b->load_image, 0, grub_efi_image_handle, NULL,
		       boot_image, size,
		       &driver_handle);
  if (status != GRUB_EFI_SUCCESS)
    {
      if (status == GRUB_EFI_OUT_OF_RESOURCES)
	grub_error (GRUB_ERR_OUT_OF_MEMORY, "out of resources");
      else
	grub_error (GRUB_ERR_BAD_OS, "cannot load image");

      goto fail;
    }
  loaded_image = grub_efi_get_loaded_image (driver_handle);
  if (! loaded_image)
    {
      grub_error (GRUB_ERR_BAD_OS, "no loaded image available");
      goto fail;
    }
  
  status = efi_call_3 (b->handle_protocol,
        driver_handle,
        &loaded_image_protocol_guid,
        (void **)&loaded_image);
  if (status != GRUB_EFI_SUCCESS)
  {
    grub_printf ("Not a dirver\n");
    goto fail;
  }
  status = efi_call_3 (b->start_image, driver_handle, NULL, NULL);
  if (status != GRUB_EFI_SUCCESS)
  {
    grub_printf ("ERROR in StartImage\n");
    goto fail;
  }
  if (connect)
  {
    status = connect_all_efi ();
    if (status != GRUB_EFI_SUCCESS)
    {
      grub_printf ("ERROR in connect_all_efi\n");
      goto fail;
    }
  }
  grub_printf ("Driver installed\n");
  return 0;
fail:
  grub_error (GRUB_ERR_BAD_OS, N_("install failed."));
  return grub_errno;
}

static grub_err_t
grub_cmd_efiload (grub_extcmd_context_t ctxt,
        int argc,
        char **args)
{
  struct grub_arg_list *state = ctxt->state;
  int connect = 1;
  grub_file_t file = 0;
  grub_efi_boot_services_t *b;
  grub_efi_status_t status;
  grub_efi_uintn_t pages = 0;
  grub_ssize_t size;
  grub_efi_physical_address_t address;
  void *boot_image = 0;
  
  b = grub_efi_system_table->boot_services;
  if (argc != 1)
    goto fail;

  file = grub_file_open (args[0], GRUB_FILE_TYPE_EFI_CHAINLOADED_IMAGE);
  if (! file)
    goto fail;
  size = grub_file_size (file);
  if (!size)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		  args[0]);
      goto fail;
    }
  pages = (((grub_efi_uintn_t) size + ((1 << 12) - 1)) >> 12);
  status = efi_call_4 (b->allocate_pages, GRUB_EFI_ALLOCATE_ANY_PAGES,
			      GRUB_EFI_LOADER_CODE,
			      pages, &address);
  if (status != GRUB_EFI_SUCCESS)
    {
      grub_printf ("Failed to allocate %u pages\n",
		    (unsigned int) pages);
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("out of memory"));
      goto fail;
    }
  boot_image = (void *) ((grub_addr_t) address);
  if (grub_file_read (file, boot_image, size) != size)
    {
      if (grub_errno == GRUB_ERR_NONE)
	grub_error (GRUB_ERR_BAD_OS, N_("premature end of file %s"),
		    args[0]);
      goto fail;
    }
  grub_file_close (file);
  if (state[0].set)
    connect = 0;
  grub_errno = grub_efi_driver_load (size, boot_image, connect);
  
  return grub_errno;
fail:
  grub_printf ("ERROR\n");
  if (file)
    grub_file_close (file);
  if (address)
    efi_call_2 (b->free_pages, address, pages);
  return 1;
}

static grub_extcmd_t cmd_efiload;

GRUB_MOD_INIT(efiload)
{
  cmd_efiload = grub_register_extcmd ("efiload", grub_cmd_efiload, 0, 
                  N_("FILE"),
                  N_("Install UEFI driver."), options_efiload);
}

GRUB_MOD_FINI(efiload)
{
  grub_unregister_extcmd (cmd_efiload);
}