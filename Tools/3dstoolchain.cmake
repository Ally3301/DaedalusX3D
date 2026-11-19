
set(DEVKITPRO $ENV{DEVKITPRO})
if(NOT DEVKITPRO)
	message(FATAL_ERROR "DEVKITPRO must point to the devkitPro installation")
endif()
set(DEVKITARM $ENV{DEVKITARM})
if(NOT DEVKITARM)
	set(DEVKITARM "${DEVKITPRO}/devkitARM")
endif()

set(CMAKE_SYSTEM_NAME "Generic")
set(CMAKE_C_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITARM}/bin/arm-none-eabi-g++")
set(CMAKE_AR "${DEVKITARM}/bin/arm-none-eabi-gcc-ar" CACHE STRING "")
set(CMAKE_RANLIB "${DEVKITARM}/bin/arm-none-eabi-gcc-ranlib" CACHE STRING "")
set(CMAKE_ASM_COMPILER "${DEVKITARM}/bin/arm-none-eabi-gcc")

set(ARCH "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -mtp=soft -D__3DS__")
set(CMAKE_C_FLAGS "${ARCH} -DNDEBUG -Wall -mword-relocations -O3 -fno-strict-aliasing -fomit-frame-pointer -fno-math-errno -fno-trapping-math -ffunction-sections -fdata-sections" CACHE STRING "C flags")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -std=gnu++14" CACHE STRING "C++ flags")
# CMake's ASM language does not reliably inherit C flags from the toolchain;
# include the ARMv6K target flags explicitly for hand-written CTR assembly.
set(CMAKE_ASM_FLAGS "${ARCH} -DNDEBUG -mword-relocations -O2" CACHE STRING "ASM flags for the Old 3DS target")
set(CMAKE_FIND_ROOT_PATH ${DEVKITARM} ${DEVKITPRO}/libctru ${DEVKITPRO}/portlibs/3ds)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "Shared libs not available")

link_directories(${DEVKITPRO}/libctru/lib ${DEVKITPRO}/portlibs/3ds/lib)