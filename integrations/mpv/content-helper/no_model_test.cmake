# The helper on a machine without the content model or its runtime: it must write state=failed with the reason and
# exit by itself (pushmpc.lua then falls back to its own measure). cmake -DHELPER=<exe> -DWORK=<folder> -P no_model_test.cmake
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/empty")
set(E "${WORK}/empty")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PMML_RUNTIME=${E}" "LOCALAPPDATA=${E}" "HOME=${E}" "XDG_DATA_HOME=${E}"
                "${HELPER}" --pid 0 --tag test --dir "${WORK}"
                RESULT_VARIABLE rc TIMEOUT 60)
if(NOT rc STREQUAL "0")
  message(FATAL_ERROR "the helper did not exit cleanly without a model (result: ${rc})")
endif()
set(st "${WORK}/pushmpc-cd-0-test.st")
if(NOT EXISTS "${st}")
  message(FATAL_ERROR "no status file ${st}")
endif()
file(READ "${st}" s)
foreach(want "pushmpc-content=1" "state=failed" "class=measuring" "animeUpscaler=0" "reason=content: image model not used")
  string(FIND "${s}" "${want}" at)
  if(at LESS 0)
    message(FATAL_ERROR "status lacks '${want}':\n${s}")
  endif()
endforeach()
message(STATUS "no model: the helper reported state=failed and exited")
