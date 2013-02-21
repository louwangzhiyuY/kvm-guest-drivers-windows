/*
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * Refer to the COPYING file for full details of the license.
 *
 * Written By: Gal Hammer <ghammer@redhat.com>
 *
 */

#include "viorng.h"
#include "viorng.tmh"

#define DOS_DEVICE_NAME  L"\\DosDevices\\HwRandom"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, VirtRngEvtDeviceAdd)
#pragma alloc_text(PAGE, VirtRngEvtDriverContextCleanup)
#endif

NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject,
                     IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    //
    // Initialize WPP Tracing
    //
    WPP_INIT_TRACING(DriverObject, RegistryPath);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "--> %!FUNC!");

    //
    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = VirtRngEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, VirtRngEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes,
        &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDriverCreate failed: %!STATUS!", status);
        WPP_CLEANUP(DriverObject);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-- %!FUNC!");

    return status;
}

NTSTATUS VirtRngEvtDeviceAdd(IN WDFDRIVER Driver,
                             IN PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
	WDF_FILEOBJECT_CONFIG fileConfig;
    WDFQUEUE readQueue;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_INTERRUPT_CONFIG interruptConfig;
    PDEVICE_CONTEXT context;

    DECLARE_CONST_UNICODE_STRING(dosDeviceName, DOS_DEVICE_NAME);

    UNREFERENCED_PARAMETER(Driver);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "--> %!FUNC!");

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    pnpPowerCallbacks.EvtDevicePrepareHardware = VirtRngEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = VirtRngEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = VirtRngEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = VirtRngEvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, WDF_NO_EVENT_CALLBACK,
		WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);

	WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig,
		WDF_NO_OBJECT_ATTRIBUTES);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDeviceCreate failed: %!STATUS!", status);
        return status;
    }

    context = GetDeviceContext(device);

    WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
        VirtRngEvtInterruptIsr, VirtRngEvtInterruptDpc);

    interruptConfig.EvtInterruptEnable = VirtRngEvtInterruptEnable;
    interruptConfig.EvtInterruptDisable = VirtRngEvtInterruptDisable;

    status = WdfInterruptCreate(device, &interruptConfig,
        WDF_NO_OBJECT_ATTRIBUTES, &context->WdfInterrupt);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfInterruptCreate failed: %!STATUS!", status);
        return status;
    }

    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES,
        &context->VirtQueueLock);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfSpinLockCreate failed: %!STATUS!", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device,
        &GUID_DEVINTERFACE_VIRT_RNG, NULL);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDeviceCreateDeviceInterface failed: %!STATUS!", status);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(device, &dosDeviceName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDeviceCreateSymbolicLink failed: %!STATUS!", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoRead = VirtRngEvtIoRead;
    queueConfig.AllowZeroLengthRequests = FALSE;

    status = WdfIoQueueCreate(device, &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES, &readQueue);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfIoQueueCreate failed: %!STATUS!", status);
        return status;
    }

    status = WdfDeviceConfigureRequestDispatching(device,
        readQueue, WdfRequestTypeRead);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDeviceConfigureRequestDispatching failed: %!STATUS!", status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-- %!FUNC!");

    return status;
}

VOID VirtRngEvtDriverContextCleanup(IN WDFOBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-> %!FUNC!");

    PAGED_CODE();

    //
    // Stop WPP Tracing
    //
    WPP_CLEANUP(WdfDriverWdmGetDriverObject(DriverObject));
}
