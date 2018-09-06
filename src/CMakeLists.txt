cmake_minimum_required(VERSION 2.8.12)

project(camlive)


#set(CMAKE_C_FLAGS "-Wall -Wextra -Wno-unused-function -Werror-implicit-function-declaration -Wno-missing-braces -Wno-missing-field-initializers ${CMAKE_C_FLAGS} -std=gnu99 -fno-strict-aliasing")
set(CMAKE_C_FLAGS "-O3 -Wall -Wextra -g")

#set(EXTRA_C_FLAGS "-march=armv8-a+crc -mfpu=neon-fp-armv8 -mtune=cortex-a53")

include_directories("/opt/vc/include/")
link_directories("/opt/vc/lib/")

set(SRC
    utils.c
    main.c
    converter.c
    rawcam.c
    )

add_executable(camlive ${SRC})

target_link_libraries(camlive mmal mmal_core mmal_util vcos bcm_host pthread)
