/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <efi.h>
#include <efilib.h>

#include "initrd.h"
#include "macro-fundamental.h"
#include "missing_efi.h"

/* extend LoadFileProtocol */
struct initrd_loader {
        EFI_LOAD_FILE_PROTOCOL load_file;
        const VOID *address;
        UINTN length;
};

/* static structure for LINUX_INITRD_MEDIA device path
   see https://github.com/torvalds/linux/blob/v5.13/drivers/firmware/efi/libstub/efi-stub-helper.c
 */
static const struct {
        VENDOR_DEVICE_PATH vendor;
        EFI_DEVICE_PATH end;
} _packed_ efi_initrd_device_path = {
        .vendor = {
                .Header = {
                        .Type = MEDIA_DEVICE_PATH,
                        .SubType = MEDIA_VENDOR_DP,
                        .Length = { sizeof(efi_initrd_device_path.vendor), 0 }
                },
                .Guid = LINUX_INITRD_MEDIA_GUID
        },
        .end = {
                .Type = END_DEVICE_PATH_TYPE,
                .SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE,
                .Length = { sizeof(efi_initrd_device_path.end), 0 }
        }
};

EFIAPI EFI_STATUS initrd_load_file(
                EFI_LOAD_FILE_PROTOCOL *this,
                EFI_DEVICE_PATH *file_path,
                BOOLEAN boot_policy,
                UINTN *buffer_size,
                VOID *buffer) {

        struct initrd_loader *loader;

        if (!this || !buffer_size || !file_path)
                return EFI_INVALID_PARAMETER;
        if (boot_policy)
                return EFI_UNSUPPORTED;

        loader = (struct initrd_loader *) this;

        if (loader->length == 0 || !loader->address)
                return EFI_NOT_FOUND;

        if (!buffer || *buffer_size < loader->length) {
                *buffer_size = loader->length;
                return EFI_BUFFER_TOO_SMALL;
        }

        CopyMem(buffer, loader->address, loader->length);
        *buffer_size = loader->length;
        return EFI_SUCCESS;
}

EFI_STATUS initrd_register(
                const VOID *initrd_address,
                UINTN initrd_length,
                EFI_HANDLE *ret_initrd_handle) {

        EFI_STATUS err;
        EFI_DEVICE_PATH *dp = (EFI_DEVICE_PATH *) &efi_initrd_device_path;
        EFI_HANDLE handle;
        struct initrd_loader *loader;

        assert(ret_initrd_handle);

        if (!initrd_address || initrd_length == 0)
                return EFI_SUCCESS;

        /* check if a LINUX_INITRD_MEDIA_GUID DevicePath is already registed.
           LocateDevicePath checks for the "closest DevicePath" and returns its handle,
           where as InstallMultipleProtocolInterfaces only maches identical DevicePaths.
         */
        err = uefi_call_wrapper(BS->LocateDevicePath, 3, &EfiLoadFile2Protocol, &dp, &handle);
        if (err != EFI_NOT_FOUND) /* InitrdMedia is already registered */
                return EFI_ALREADY_STARTED;

        loader = AllocatePool(sizeof(struct initrd_loader));
        if (!loader)
                return EFI_OUT_OF_RESOURCES;

        *loader = (struct initrd_loader) {
                .load_file.LoadFile = initrd_load_file,
                .address = initrd_address,
                .length = initrd_length
        };

        /* create a new handle and register the LoadFile2 protocol with the InitrdMediaPath on it */
        err = uefi_call_wrapper(
                        BS->InstallMultipleProtocolInterfaces, 8,
                        ret_initrd_handle,
                        &DevicePathProtocol, &efi_initrd_device_path,
                        &EfiLoadFile2Protocol, loader,
                        NULL);
        if (EFI_ERROR(err))
                FreePool(loader);

        return err;
}

EFI_STATUS initrd_unregister(EFI_HANDLE initrd_handle) {
        EFI_STATUS err;
        struct initrd_loader *loader;

        if (!initrd_handle)
                return EFI_SUCCESS;

        /* get the LoadFile2 protocol that we allocated earlier */
        err = uefi_call_wrapper(
                        BS->OpenProtocol, 6,
                        initrd_handle, &EfiLoadFile2Protocol, (VOID **) &loader,
                        NULL, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(err))
                return err;

        /* close the handle */
        (void) uefi_call_wrapper(
                        BS->CloseProtocol, 4,
                        initrd_handle, &EfiLoadFile2Protocol, NULL, NULL);

        /* uninstall all protocols thus destroying the handle */
        err = uefi_call_wrapper(
                        BS->UninstallMultipleProtocolInterfaces, 6,
                        initrd_handle,
                        &DevicePathProtocol, &efi_initrd_device_path,
                        &EfiLoadFile2Protocol, loader,
                        NULL);
        if (EFI_ERROR(err))
                return err;

        initrd_handle = NULL;
        FreePool(loader);
        return EFI_SUCCESS;
}
