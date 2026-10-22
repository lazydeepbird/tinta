if(NOT DEFINED TEST_BINARY OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "TEST_BINARY and TEST_DIR are required")
endif()
# Copy the test next to its own settings.ini: that makes the folder the
# portable configuration home, so the test never reads or writes %APPDATA%.
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")
get_filename_component(TEST_NAME "${TEST_BINARY}" NAME)
file(COPY "${TEST_BINARY}" DESTINATION "${TEST_DIR}")
file(WRITE "${TEST_DIR}/settings.ini" "[Settings]\nhasAskedFileAssociation=1\ncheckUpdates=0\n")
execute_process(COMMAND "${TEST_DIR}/${TEST_NAME}"
    WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${TEST_NAME} failed: ${result}")
endif()
