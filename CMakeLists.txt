cmake_minimum_required(VERSION 3.5)

project(sdr_usb_gadget LANGUAGES C)

# Options
option(GENERATE_STATS "Generate and output runtime stats" OFF)

# From: https://www.mattkeeter.com/blog/2018-01-06-versioning/
execute_process(COMMAND git log --pretty=format:'%h' -n 1
                OUTPUT_VARIABLE GIT_REV
                ERROR_QUIET)
if ("${GIT_REV}" STREQUAL "")
    set(GIT_REV "N/A")
    set(GIT_DIFF "")
else()
    execute_process(
        COMMAND bash -c "git diff --quiet --exit-code || echo +"
        OUTPUT_VARIABLE GIT_DIFF)

    string(STRIP "${GIT_REV}" GIT_REV)
    #string(SUBSTRING "${GIT_REV}" 1 7 GIT_REV)
    string(STRIP "${GIT_DIFF}" GIT_DIFF)
endif()
set(GIT_VERSION "${GIT_REV}${GIT_DIFF}")

if (NOT "${GIT_VERSION_OVERRIDE}" STREQUAL "")
    # Use provided override if package isn't being built within git repo
    set(GIT_VERSION "${GIT_VERSION_OVERRIDE}")
endif()

add_executable(sdr_usb_gadget
    main.c
    usb_descriptors.c
    epoll_loop.c
    ring_buffer.c
    thread_read.c
    thread_write.c
    utils.c
)
target_link_libraries(sdr_usb_gadget
    pthread
    aio
    iio
)
target_compile_definitions(sdr_usb_gadget PRIVATE
    PROGRAM_VERSION="${GIT_VERSION}"
    STATS_PERIOD_SECS=2)
if (GENERATE_STATS)
target_compile_definitions(sdr_usb_gadget PRIVATE GENERATE_STATS=1)
endif(GENERATE_STATS)

install(TARGETS sdr_usb_gadget RUNTIME DESTINATION sbin)
