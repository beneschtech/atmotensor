# Probe used by the top-level CMakeLists: can eccodes' CMake config package
# be loaded at all?  Some distro packages ship a config whose import checks
# fail fatally (e.g. Ubuntu's references libeccodes_f90.so.0, which is not
# installed), so the main configure only calls find_package(eccodes CONFIG)
# after this sub-configure succeeded.
cmake_minimum_required(VERSION 3.25)
project(atmo_eccodes_probe LANGUAGES C)
find_package(eccodes CONFIG REQUIRED)
if(NOT TARGET eccodes)
  message(FATAL_ERROR "eccodes config package has no 'eccodes' target")
endif()
