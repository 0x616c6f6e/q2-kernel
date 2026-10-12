# Shared by standalone CMake and SoC consumers; paths are relative to Q2-Kernel.
set(Q2_CORE_SOURCES src/core/result.c src/core/kernel.c src/core/log.c components/input/keys.c components/data/data.c components/network/network.c components/audio/audio.c)
set(Q2_RUNTIME_SOURCES src/runtime/manifest.c src/runtime/lua_runtime.c components/apps/apps.c)
get_filename_component(Q2_VENDOR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party" ABSOLUTE)
file(GLOB Q2_LUA_SOURCES "${Q2_VENDOR_ROOT}/lua/*.c")
foreach(name lua luac linit liolib loslib loadlib ldblib lcorolib)
  list(REMOVE_ITEM Q2_LUA_SOURCES "${Q2_VENDOR_ROOT}/lua/${name}.c")
endforeach()
set(Q2_STORAGE_SOURCES components/storage/storage.c components/storage/fatfs.c
    third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
set(Q2_SQLITE_SOURCES components/storage/sqlite.c components/storage/sqlite_memory_vfs.c third_party/sqlite/sqlite3.c)
set(Q2_SQLITE_DEFINITIONS SQLITE_OS_OTHER=1 SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_TEMP_STORE=3 SQLITE_DEFAULT_MEMSTATUS=0)
set(Q2_USB_SOURCES components/usb/usb.c third_party/tinyusb/src/tusb.c
    third_party/tinyusb/src/common/tusb_fifo.c third_party/tinyusb/src/device/usbd.c
    third_party/tinyusb/src/device/usbd_control.c third_party/tinyusb/src/class/cdc/cdc_device.c)
