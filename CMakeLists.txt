project("ffmpeg4-turial")
cmake_minimum_required(VERSION 3.4.0)

link_libraries(m)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

set(FFMPEG_LIBS "avutil;avcodec;avformat;swscale;swresample;avdevice")

file(GLOB_RECURSE mains RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/turial*")
foreach(mainfile IN LISTS mains)
    # Get file name without directory
    get_filename_component(mainname ${mainfile} NAME_WE)
    add_executable(${mainname} ${mainfile})
    target_link_libraries(${mainname} ${SDL2_LIBRARIES} ${FFMPEG_LIBS})
endforeach()

add_executable(PurePlayer PurePlayer.c)
target_link_libraries(PurePlayer ${SDL2_LIBRARIES} ${FFMPEG_LIBS})
