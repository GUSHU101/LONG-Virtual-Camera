cmake_minimum_required(VERSION 3.22)
project(LONGVirtualCameraMobile LANGUAGES NONE)

message(STATUS
  "LONG Virtual Camera is Android-only. Configure the native payload with "
  "-S ${CMAKE_CURRENT_LIST_DIR}/android and an Android NDK toolchain, then "
  "build the APK from ../../lsposed-module.")
