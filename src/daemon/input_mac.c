#include "command.h"
#include "device.h"
#include "input.h"

#ifdef OS_MAC
#ifndef OS_MAC_LEGACY
#include <errno.h>
#include <sys/sysctl.h>
#include "input_mac_vhid.h"

int osx_ver = -1;
// VirtualHIDKeyboard
vhid_kbinput kbinput_key = {1, {0}, 0, {{0}}};
vhid_kbinput kbinput_consumer = {2, {0}, 0, {{0}}};
vhid_kbinput kbinput_avtopcase = {3, {0}, 0, {{0}}};
vhid_kbinput kbinput_vendor = {4, {0}, 0, {{0}}};
//vhid_kbconsumerinput kbinput_consumer = {2, {{0}}};
//vhid_kbavtopcaseinput kbinput_avtopcase = {3, {{0}}};

// VirtualHIDPointing
vhid_mouseinput mouseinput = {{0}, 0, 0, 0, 0};

int os_inputopen(usbdevice* kb){
    // Check if Karabiner VirtualHIDDevice is loaded
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceNameMatching(VIRTUAL_HID_ROOT_NAME));
    if(!service)
        ckb_fatal("Unable to open VirtualHIDDevice\n");

    kern_return_t kr;
    kr = IOServiceOpen(service, mach_task_self(), kIOHIDServerConnectType, &(kb->event));
    if(kr != KERN_SUCCESS)
        ckb_fatal("IOServiceOpen for VirtualHIDKeyboard error\n");

    kr = IOServiceOpen(service, mach_task_self(), kIOHIDServerConnectType, &(kb->event_mouse));
    if(kr != KERN_SUCCESS)
        ckb_fatal("IOServiceOpen for VirtualHIDPointing error\n");

    vhid_properties properties = {0};
    
    kr = IOConnectCallStructMethod(kb->event,
                                    initialize_virtual_hid_keyboard,
                                    &properties, sizeof(vhid_properties),
                                    NULL, 0);
    if(kr != KERN_SUCCESS)
        ckb_fatal("VirtualHIDKeyboard init error\n");

    while(1){
        ckb_info("Waiting for VirtualHIDKeyboard...\n");

        bool ready = false;
        size_t readysize = sizeof(ready);
        kr = IOConnectCallStructMethod(kb->event,
                                    is_virtual_hid_keyboard_ready,
                                    NULL, 0,
                                    &ready, &readysize);
        if (kr != KERN_SUCCESS) {
            ckb_err("VirtualHIDKeyboard not ready yet\n");
        } else {
            if(ready){
                ckb_info("VirtualHIDKeyboard ready\n");
                break;
            }
        }
        clock_nanosleep(CLOCK_MONOTONIC, 0, &(struct timespec) {.tv_nsec = 10000000}, NULL);
    }
    
/* MOUSE */
    
    kr = IOConnectCallStructMethod(kb->event_mouse,
                                    initialize_virtual_hid_pointing,
                                    NULL, 0,
                                    NULL, 0);
    if(kr != KERN_SUCCESS)
        ckb_fatal("VirtualHIDPointing init error %x\n", kr);

    clock_nanosleep(CLOCK_MONOTONIC, 0, &(struct timespec) {.tv_nsec = 10000000}, NULL);

    return 0;
}

void os_inputclose(usbdevice* kb){
    if(kb->event){
        IOServiceClose(kb->event);
        kb->event = 0;
    }
}

// Unlike Linux, OSX keyboards have independent caps lock states. This means they're set by the driver itself so we don't poll for external events.
// However, updating indicator state requires locking dmutex and we never want to do that in the input thread.
// Instead, we launch a single-shot thread to update the state.
static void* indicator_update(void* context){
    usbdevice* kb = context;
    pthread_mutex_lock(dmutex(kb));
    {
        pthread_mutex_lock(imutex(kb));
        IOOptionBits modifiers = kb->modifiers;
        // Allow the thread to be spawned again
        kb->indicthread = 0;
        pthread_mutex_unlock(imutex(kb));
        // Num lock on, Caps dependent on modifier state
        uchar ileds = 1 | !!(modifiers & NX_ALPHASHIFTMASK) << 1;
        kb->hw_ileds = ileds;
        kb->vtable->updateindicators(kb, 0);
    }
    pthread_mutex_unlock(dmutex(kb));
    return 0;
}

void os_keypress(usbdevice* kb, int scancode, int down){
    if(scancode & SCAN_MOUSE){
        if(scancode == BTN_WHEELUP || scancode == BTN_WHEELDOWN) {
            mouseinput.vertical_wheel = (down ? ((scancode == BTN_WHEELUP) ? 1 : -1) : 0);
        } else {
            int scan = (scancode & ~SCAN_MOUSE);
            if(down)
                add_to_buttons(scan, &mouseinput);
            else
                remove_from_buttons(scan, &mouseinput);
        }
        IOConnectCallStructMethod(kb->event_mouse, post_pointing_input_report, &mouseinput, sizeof(mouseinput), NULL, 0);
        return;
    }
    ckb_info("scancode %x\n", scancode);
    // Pick the appropriate add or remove function
    add_remove_keys = down ? &add_to_keys : &remove_from_keys;
    
    if(IS_VENDOR(scancode)) { // Fn
        scancode = scancode - KEY_CONSUMER;
        (*add_remove_keys)(scancode, &(kbinput_vendor.keys));
        ckb_info("Vendor\n %i", scancode);
        IOConnectCallStructMethod(kb->event, post_keyboard_input_report, &kbinput_vendor, sizeof(kbinput_vendor), NULL, 0);
    } else if(IS_CONSUMER(scancode)){
        scancode = scancode - KEY_CONSUMER;
        (*add_remove_keys)(scancode, &(kbinput_consumer.keys));
        IOConnectCallStructMethod(kb->event, post_keyboard_input_report, &kbinput_consumer, sizeof(kbinput_consumer), NULL, 0);
    } else {
        (*add_remove_keys)(scancode, &(kbinput_key.keys));
        IOConnectCallStructMethod(kb->event, post_keyboard_input_report, &kbinput_key, sizeof(kbinput_key), NULL, 0);
    }
}

void os_mousemove(usbdevice* kb, int x, int y){
    mouseinput.x = x;
    mouseinput.y = y;
    IOConnectCallStructMethod(kb->event_mouse, post_pointing_input_report, &mouseinput, sizeof(mouseinput), NULL, 0);
}

int os_setupindicators(usbdevice* kb){
    // Set NumLock on permanently
    kb->hw_ileds = kb->hw_ileds_old = kb->ileds = 1;
    return 0;
}

#endif
#endif
