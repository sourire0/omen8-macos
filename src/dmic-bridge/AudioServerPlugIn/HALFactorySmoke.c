#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>

typedef void *(*HPIPC4Factory)(CFAllocatorRef, CFUUIDRef);

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    CFStringRef path = CFStringCreateWithCString(
        NULL, argv[1], kCFStringEncodingUTF8);
    if (!path) return 3;
    CFURLRef url = CFURLCreateWithFileSystemPath(
        NULL, path, kCFURLPOSIXPathStyle, true);
    if (!url) {
        CFRelease(path);
        return 3;
    }
    CFBundleRef bundle = CFBundleCreate(NULL, url);
    if (!bundle || !CFBundleLoadExecutable(bundle)) {
        fprintf(stderr, "CFBundle load failed\n");
        if (bundle) CFRelease(bundle);
        CFRelease(url);
        CFRelease(path);
        return 4;
    }

    HPIPC4Factory factory = (HPIPC4Factory)CFBundleGetFunctionPointerForName(
        bundle, CFSTR("HPIPC4Audio_Create"));
    if (!factory) {
        fprintf(stderr, "declared factory symbol unavailable\n");
        CFBundleUnloadExecutable(bundle);
        CFRelease(bundle);
        CFRelease(url);
        CFRelease(path);
        return 5;
    }

    AudioServerPlugInDriverRef driver = (AudioServerPlugInDriverRef)factory(
        kCFAllocatorDefault, kAudioServerPlugInTypeUUID);
    if (!driver || !*driver) {
        fprintf(stderr, "factory rejected AudioServerPlugIn type UUID\n");
        CFBundleUnloadExecutable(bundle);
        CFRelease(bundle);
        CFRelease(url);
        CFRelease(path);
        return 6;
    }

    CFUUIDBytes interfaceID =
        CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID);
    void *interface = NULL;
    const HRESULT queryResult =
        (*driver)->QueryInterface(driver, interfaceID, &interface);
    if (queryResult != 0 || interface != driver) {
        fprintf(stderr, "driver interface query failed: 0x%x\n",
                (unsigned)queryResult);
        (*driver)->Release(driver);
        CFBundleUnloadExecutable(bundle);
        CFRelease(bundle);
        CFRelease(url);
        CFRelease(path);
        return 7;
    }


    const struct {
        AudioObjectID object;
        AudioObjectPropertyAddress property;
    } sources[] = {
        {3, {kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeInput, 0}},
        {3, {kAudioDevicePropertyDataSources, kAudioObjectPropertyScopeInput, 0}},
        {7, {kAudioSelectorControlPropertyCurrentItem, kAudioObjectPropertyScopeGlobal, 0}},
        {7, {kAudioSelectorControlPropertyAvailableItems, kAudioObjectPropertyScopeGlobal, 0}},
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
        UInt32 value[4] = {0};
        UInt32 size = 0;
        if ((*driver)->GetPropertyDataSize(driver, sources[i].object, 0,
                                         &sources[i].property, 0, NULL, &size) ||
            size != sizeof(UInt32) ||
            (*driver)->GetPropertyData(driver, sources[i].object, 0,
                                       &sources[i].property, 0, NULL,
                                       sizeof(value), &size, value) ||
            size != sizeof(UInt32) || value[0] != 'imic') {
            fprintf(stderr, "Internal microphone property %zu was not published correctly\n", i);
            return 8;
        }
    }
    printf("Single internal microphone source (imic): PASS\n");

    (*driver)->Release(interface);
    printf("HAL bundle load, factory, and driver interface: PASS\n");
    printf("Initialize/StartIO not called; no IOKit client or audio path opened.\n");
    CFBundleUnloadExecutable(bundle);
    CFRelease(bundle);
    CFRelease(url);
    CFRelease(path);
    return 0;
}
