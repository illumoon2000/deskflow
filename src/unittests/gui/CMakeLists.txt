# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME VersionCheckerTests
  DEPENDS gui
  SOURCE VersionCheckerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME FileSharingTests
  DEPENDS gui
  SOURCE FileSharingTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME FileSharingUiTests
  DEPENDS gui
  SOURCE FileSharingUiTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(FileSharingTests PROPERTIES TIMEOUT 90)
