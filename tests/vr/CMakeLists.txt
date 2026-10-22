cmake_minimum_required(VERSION 3.18)
project(VRBodyTrackingTests LANGUAGES CXX)

# Standalone: no headset, OpenXR loader or CppUnit installation required.
find_path(GLM_INCLUDE_DIR glm/glm.hpp REQUIRED)
add_executable(vr-body-tracking-tests BodyTrackingTest.cpp)
target_compile_features(vr-body-tracking-tests PRIVATE cxx_std_17)
target_include_directories(vr-body-tracking-tests PRIVATE ../../src "${GLM_INCLUDE_DIR}")
enable_testing()
add_test(NAME vr-body-tracking COMMAND vr-body-tracking-tests)
