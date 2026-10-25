# Differential test of an override on tonc's irq_demo (tests/overrides/difftest): the correct
# override passes 10,000 random states with full block coverage, a deliberately wrong one (off by
# one word) fails with a minimal diff, and states recorded during a headless boot pass.
# `anygba difftest --mark-verified` then records the pass in a scratch copy of the module.
#   cmake -DGAME=<exe> -DROM=<rom> -DSPEC=<difftest.toml> -DANYGBA=<anygba exe> -DWORK=<dir>
#     -P override-difftest.cmake
foreach(var GAME ROM SPEC ANYGBA WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()
file(MAKE_DIRECTORY "${WORK}")

function(run_game expected_rc out_var)
  execute_process(COMMAND "${GAME}" "${ROM}" ${ARGN}
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL expected_rc)
    message(FATAL_ERROR "${ARGN}: exit code ${rc}, expected ${expected_rc}:\n${out}\n${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

function(expect_json file expected)
  file(READ "${WORK}/${file}" json)
  string(JSON value GET "${json}" ${ARGN})
  if(NOT value STREQUAL "${expected}")
    message(FATAL_ERROR "${file}: ${ARGN} is '${value}', expected '${expected}'")
  endif()
endfunction()

# The correct override: 10,000 random states, ABI and full state identical, every block run.
run_game(0 out --difftest 0x08000186 --spec "${SPEC}" --states 10000 --json pass.json)
message(STATUS "${out}")
expect_json(pass.json ON pass)
expect_json(pass.json 10000 states total)
expect_json(pass.json 10000 states full_identical)
file(READ "${WORK}/pass.json" json)
string(JSON percent GET "${json}" coverage percent)
if(percent LESS 90)
  message(FATAL_ERROR "block coverage ${percent}% is below 90%")
endif()

# Off by one: caught, and the first failing state shrinks to a zero byte count where the wrong loop
# still clears one word.
run_game(1 out --difftest 0x08000186 --impl difftest_zero_fill_off_by_one --spec "${SPEC}" --states 2000
  --json fail.json)
message(STATUS "${out}")
expect_json(fail.json OFF pass)
expect_json(fail.json 0x00000000 first_failure minimised_input r 1)
file(READ "${WORK}/fail.json" json)
string(JSON diffs GET "${json}" first_failure minimised_differences)
if(NOT diffs MATCHES "original 0 writes, override 1;")
  message(FATAL_ERROR "the minimised diff does not show the one extra write:\n${diffs}")
endif()

# Recorded states: crt0 clears its sections at boot.
file(REMOVE "${WORK}/boot.states")
run_game(0 out --headless --frames 5 --no-save --no-miss-log --record-entry 0x08000186 --count 8
  --record-out boot.states)
if(NOT out MATCHES "record: [1-9][0-9]* entry states of 0x08000186 written")
  message(FATAL_ERROR "no states recorded:\n${out}")
endif()
run_game(0 out --difftest 0x08000186 --recorded boot.states --states 0 --json recorded.json)
message(STATUS "${out}")
expect_json(recorded.json ON pass)

# The anygba front end: spec from the module, pass recorded as verified = true in its overrides.toml.
get_filename_component(module_src "${SPEC}" DIRECTORY)
file(REMOVE_RECURSE "${WORK}/module")
file(MAKE_DIRECTORY "${WORK}/module")
file(COPY "${module_src}/difftest.toml" DESTINATION "${WORK}/module")
file(READ "${module_src}/overrides.toml" toml)
string(REGEX REPLACE "verified = true[^\n]*\n" "" toml "${toml}")
file(WRITE "${WORK}/module/overrides.toml" "${toml}")
execute_process(COMMAND "${ANYGBA}" difftest "${GAME}" --rom "${ROM}" --func 0x08000186 --overrides "${WORK}/module"
    --states 500 --recorded "${WORK}/boot.states" --mark-verified
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "anygba difftest --mark-verified: exit code ${rc}:\n${out}\n${err}")
endif()
file(READ "${WORK}/module/overrides.toml" marked)
if(NOT marked MATCHES "impl = \"difftest_zero_fill\"\r?\nverified = true  # anygba difftest passed [0-9-]+: 503 states, block coverage 100.0%")
  message(FATAL_ERROR "verified = true was not recorded:\n${marked}")
endif()
execute_process(COMMAND "${ANYGBA}" difftest "${GAME}" --rom "${ROM}" --func 0x08000186 --overrides "${WORK}/module"
    --impl difftest_zero_fill_off_by_one --states 200
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 1)
  message(FATAL_ERROR "anygba difftest on the wrong implementation: exit code ${rc}, expected 1:\n${out}\n${err}")
endif()
