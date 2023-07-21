#!/usr/bin/cmake -P

CMAKE_PATH(GET CMAKE_SCRIPT_MODE_FILE PARENT_PATH SCRIPT_DIR)

SET(ENV{DESTDIR} ${SCRIPT_DIR}/install/chan-quectel)
EXECUTE_PROCESS(
    COMMAND ${CMAKE_COMMAND} --install build --component chan-quectel --prefix=/usr
    WORKING_DIRECTORY ${SCRIPT_DIR}
    COMMAND_ERROR_IS_FATAL ANY
)
