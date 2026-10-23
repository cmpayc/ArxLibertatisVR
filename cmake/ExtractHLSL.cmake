
# Extract HLSL source code from the raw string literals of a C++ header
#
# Usage: cmake -DINPUT=<header> -DOUTPUT=<hlsl file> "-DARRAYS=<name>;<name>..." -P ExtractHLSL.cmake
#
# Each array is a 'static const char <name>[] = R"HLSL(...)HLSL" R"HLSL(...)HLSL";' definition that can be split into
# several literals. The source code of all arrays is concatenated in the given order.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED ARRAYS)
	message(FATAL_ERROR "Usage: cmake -DINPUT=... -DOUTPUT=... -DARRAYS=... -P ExtractHLSL.cmake")
endif()

file(READ "${INPUT}" header)
string(REPLACE "\r" "" header "${header}")

set(source "")
foreach(array IN LISTS ARRAYS)

	string(FIND "${header}" "static const char ${array}[] = R\"HLSL(" begin)
	if(begin EQUAL -1)
		message(FATAL_ERROR "${INPUT}: ${array} not found")
	endif()
	string(SUBSTRING "${header}" ${begin} -1 code)
	string(FIND "${code}" "R\"HLSL(" start)
	math(EXPR start "${start} + 7")
	string(SUBSTRING "${code}" ${start} -1 code)
	string(FIND "${code}" ")HLSL\";" end)
	if(end EQUAL -1)
		message(FATAL_ERROR "${INPUT}: end of ${array} not found")
	endif()
	string(SUBSTRING "${code}" 0 ${end} code)

	# Join the separate literals, keeping the line count
	string(REGEX REPLACE "\\)HLSL\"[^\n]*\nR\"HLSL\\(" "\n" code "${code}")

	string(APPEND source "${code}\n")

endforeach()

# Only touch the output if it changed so that the shaders are not recompiled needlessly
if(EXISTS "${OUTPUT}")
	file(READ "${OUTPUT}" old_source)
	if(old_source STREQUAL source)
		return()
	endif()
endif()
file(WRITE "${OUTPUT}" "${source}")
