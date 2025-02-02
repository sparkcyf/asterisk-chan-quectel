#!/usr/bin/cmake -P

CMAKE_PATH(GET CMAKE_SCRIPT_MODE_FILE PARENT_PATH SCRIPT_DIR)

EXECUTE_PROCESS(
    COMMAND ${CMAKE_COMMAND} --build --preset default
    WORKING_DIRECTORY ${SCRIPT_DIR}
    COMMAND_ERROR_IS_FATAL ANY
)
